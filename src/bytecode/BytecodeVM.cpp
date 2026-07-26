//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/BytecodeVM.cpp
// Purpose: Execute lowered Zanna bytecode programs and bridge runtime calls.
// Key invariants:
//   - Bytecode stack, frame, and string ownership metadata remain synchronized.
//   - Runtime bridge calls preserve VM trap semantics and deterministic state.
// Ownership/Lifetime:
//   - BytecodeVM borrows loaded BytecodeModule storage for the duration of execution.
//   - Runtime callback bridges borrow active VM/module state only for synchronous calls.
// Links: src/bytecode/BytecodeVM.hpp, src/vm/RuntimeBridge.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements bytecode validation, interpretation, traps, debugging, and
 *        callback-aware runtime dispatch for `BytecodeVM`.
 *
 * The VM has two dispatch implementations: the portable switch loop in this
 * file and the computed-goto loop in `BytecodeVM_threaded.cpp`. Both share the
 * frame, operand-stack, string-ownership, memory-safety, and exception state
 * maintained here. This file also adapts runtime APIs whose callbacks may refer
 * to either tree-walker IL functions or tagged bytecode functions.
 *
 * Loaded modules are borrowed during synchronous execution. Runtime operations
 * that may outlive the caller instead capture shared module snapshots and an
 * `ExecutionEnvironment`, ensuring that worker callbacks never retain pointers
 * into a caller-owned module.
 */

#include "bytecode/BytecodeVM.hpp"
#include "bytecode/BytecodeSemantics.hpp"
#include "il/core/Module.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "rt_async.h"
#include "rt_future.h"
#include "rt_game3d.h"
#include "rt_lazy.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_http_server.h"
#include "rt_https_server.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_threadpool.h"
#include "rt_threads.h"
#include "support/small_vector.hpp"
#include "zanna/runtime/rt.h"
#include "vm/OpHandlerAccess.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/VM.hpp"
#include "vm/VMContext.hpp"
#include "vm/err_bridge.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zanna {
namespace bytecode {

namespace {

/// @brief True if parameter @p index of runtime signature @p sig is a string,
///        so the caller knows that argument participates in string ownership.
/// @param sig Runtime signature to inspect; may be null.
/// @param index Zero-based parameter index.
/// @return `true` only when the indexed parameter exists and has string type.
bool runtimeParamIsString(const il::runtime::RuntimeSignature *sig, size_t index) {
    return sig && index < sig->paramTypes.size() &&
           sig->paramTypes[index].kind == il::core::Type::Kind::Str;
}

/// @brief Install callback-aware handlers shared by the tree-walker and bytecode VMs.
void registerUnifiedVmRuntimeHandlers();

/// @brief No-op runtime trap observer; installed so the runtime's trap-signal
///        hook has a valid target while the bytecode VM handles traps itself.
/// @details Both callback arguments are intentionally ignored.
void bytecodeRuntimeTrapPassthrough(const il::vm::RuntimeTrapSignal &, void *) {}

/// @brief Default numeric error code for a trap kind when none is supplied
///        (DivideByZero→0, Overflow→4, Bounds→7, NullPointer→91, else 0).
/// @param trapKind Trap category whose conventional code is requested.
/// @return Stable runtime error code associated with @p trapKind.
int32_t defaultBytecodeTrapErrorCode(TrapKind trapKind) {
    switch (trapKind) {
        case TrapKind::DivideByZero:
            return 0;
        case TrapKind::Overflow:
            return 4;
        case TrapKind::Bounds:
            return 7;
        case TrapKind::NullPointer:
            return 91;
        default:
            return 0;
    }
}

/// @brief Stable human-readable name for a trap kind (for diagnostics/traces);
///        returns "Unknown" for any value outside the enum.
/// @param trapKind Trap category to format.
/// @return Pointer to a static, null-terminated name.
const char *bytecodeTrapKindName(TrapKind trapKind) {
    switch (trapKind) {
        case TrapKind::DivideByZero:
            return "DivideByZero";
        case TrapKind::Overflow:
            return "Overflow";
        case TrapKind::InvalidCast:
            return "InvalidCast";
        case TrapKind::DomainError:
            return "DomainError";
        case TrapKind::Bounds:
            return "Bounds";
        case TrapKind::FileNotFound:
            return "FileNotFound";
        case TrapKind::EndOfFile:
            return "EndOfFile";
        case TrapKind::IOError:
            return "IOError";
        case TrapKind::InvalidOperation:
            return "InvalidOperation";
        case TrapKind::RuntimeError:
            return "RuntimeError";
        case TrapKind::Interrupt:
            return "Interrupt";
        case TrapKind::NetworkError:
            return "NetworkError";
        case TrapKind::NullPointer:
            return "NullPointer";
        case TrapKind::StackOverflow:
            return "StackOverflow";
        case TrapKind::InvalidOpcode:
            return "InvalidOpcode";
        case TrapKind::None:
            return "None";
    }
    return "Unknown";
}

using UnifiedRuntimeHandler = void (*)(void **, void *);
std::once_flag gUnifiedRuntimeHandlersOnce;
UnifiedRuntimeHandler gPriorThreadStartHandler = nullptr;
UnifiedRuntimeHandler gPriorThreadStartOwnedHandler = nullptr;
UnifiedRuntimeHandler gPriorThreadStartSafeHandler = nullptr;
UnifiedRuntimeHandler gPriorThreadStartSafeOwnedHandler = nullptr;
UnifiedRuntimeHandler gPriorAsyncRunHandler = nullptr;
UnifiedRuntimeHandler gPriorPoolSubmitOwnedHandler = nullptr;
UnifiedRuntimeHandler gPriorHttpBindHandler = nullptr;
UnifiedRuntimeHandler gPriorHttpsBindHandler = nullptr;
UnifiedRuntimeHandler gPriorGame3DRunHandler = nullptr;
UnifiedRuntimeHandler gPriorGame3DRunWithOverlayHandler = nullptr;
UnifiedRuntimeHandler gPriorGame3DRunFixedHandler = nullptr;
UnifiedRuntimeHandler gPriorGame3DRunFixedWithOverlayHandler = nullptr;
UnifiedRuntimeHandler gPriorGame3DRunFramesHandler = nullptr;
UnifiedRuntimeHandler gPriorGame3DDrawOverlayHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelForHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelForPoolHandler = nullptr;
UnifiedRuntimeHandler gPriorPoolSubmitHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelInvokeHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelInvokePoolHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelForEachHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelForEachPoolHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelMapHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelMapPoolHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelReduceHandler = nullptr;
UnifiedRuntimeHandler gPriorParallelReducePoolHandler = nullptr;
UnifiedRuntimeHandler gPriorLazyNewHandler = nullptr;
UnifiedRuntimeHandler gPriorLazyGetHandler = nullptr;
UnifiedRuntimeHandler gPriorLazyGetStrHandler = nullptr;
UnifiedRuntimeHandler gPriorLazyGetI64Handler = nullptr;
UnifiedRuntimeHandler gPriorLazyForceHandler = nullptr;
UnifiedRuntimeHandler gPriorLazyMapHandler = nullptr;
UnifiedRuntimeHandler gPriorLazyAndThenHandler = nullptr;
UnifiedRuntimeHandler gPriorOptionMapHandler = nullptr;
UnifiedRuntimeHandler gPriorOptionAndThenHandler = nullptr;
UnifiedRuntimeHandler gPriorOptionOrElseHandler = nullptr;
UnifiedRuntimeHandler gPriorOptionFilterHandler = nullptr;
UnifiedRuntimeHandler gPriorResultMapHandler = nullptr;
UnifiedRuntimeHandler gPriorResultMapErrHandler = nullptr;
UnifiedRuntimeHandler gPriorResultAndThenHandler = nullptr;
UnifiedRuntimeHandler gPriorResultOrElseHandler = nullptr;

} // namespace

//===----------------------------------------------------------------------===//
// Thread-local active BytecodeVM tracking
//===----------------------------------------------------------------------===//

/// Thread-local pointer to the currently active BytecodeVM.
/// This enables runtime handlers (like Thread.Start) to detect when they're
/// being called from bytecode execution and handle threading correctly.
thread_local BytecodeVM *tlsActiveBytecodeVM = nullptr;

/// Thread-local pointer to the current BytecodeModule (for thread spawning).
thread_local const BytecodeModule *tlsActiveBytecodeModule = nullptr;

/// @brief The BytecodeVM executing on the current thread, or NULL — lets
///        runtime handlers detect a bytecode caller and adapt accordingly.
/// @return Borrowed thread-local VM pointer, or null outside bytecode execution.
BytecodeVM *activeBytecodeVMInstance() {
    return tlsActiveBytecodeVM;
}

/// @brief The BytecodeModule active on the current thread (used when spawning
///        worker threads that must run bytecode entry points).
/// @return Borrowed thread-local module pointer, or null when no module is active.
const BytecodeModule *activeBytecodeModule() {
    return tlsActiveBytecodeModule;
}

namespace {

/// @brief RAII guard for the active bytecode module thread-local.
/// @details Runtime shims use the active module to resolve tagged bytecode
///          function pointers when spawning threads, async workers, or HTTP
///          handlers. The guard restores any previous module for re-entrant
///          bytecode execution.
class ActiveBytecodeModuleGuard final {
  public:
    /// @brief Publish @p module as the active bytecode module for this thread.
    /// @param module Borrowed module to expose until this guard is destroyed.
    explicit ActiveBytecodeModuleGuard(const BytecodeModule *module)
        : previous_(tlsActiveBytecodeModule) {
        tlsActiveBytecodeModule = module;
    }

    /// @brief Restore the module that was active before construction.
    ~ActiveBytecodeModuleGuard() {
        tlsActiveBytecodeModule = previous_;
    }

    ActiveBytecodeModuleGuard(const ActiveBytecodeModuleGuard &) = delete;
    ActiveBytecodeModuleGuard &operator=(const ActiveBytecodeModuleGuard &) = delete;

  private:
    const BytecodeModule *previous_ = nullptr; ///< Previous thread-local module.
};

} // namespace

/// @brief RAII: make @p vm the thread-active VM for the guard's lifetime,
///        restoring the previous one on scope exit (supports re-entrant
///        bytecode execution, e.g. a runtime callback into bytecode).
/// @param vm Borrowed VM to expose on the current thread.
ActiveBytecodeVMGuard::ActiveBytecodeVMGuard(BytecodeVM *vm)
    : previous_(tlsActiveBytecodeVM), current_(vm) {
    tlsActiveBytecodeVM = vm;
}

/// @brief Restore the previously-active thread VM.
ActiveBytecodeVMGuard::~ActiveBytecodeVMGuard() {
    tlsActiveBytecodeVM = previous_;
}

/// @brief Construct an idle VM with pre-allocated execution buffers.
/// @details The value stack and per-slot string-ownership bitmap are sized for
///          the worst case up front, and the alloca arena reserves its full
///          16 MiB so it never reallocates mid-run — alloca pointers live in
///          registers/operand slots and must stay valid for the function's
///          lifetime. Threaded dispatch is the default.
BytecodeVM::BytecodeVM()
    : module_(nullptr), state_(VMState::Ready), trapKind_(TrapKind::None), currentErrorCode_(0),
      sp_(nullptr), fp_(nullptr), instrCount_(0), maxInstrCount_(0), runtimeBridgeEnabled_(false),
      useThreadedDispatch_(true), // Default to faster threaded dispatch
      trustedDispatch_(false), reentrantStopDepth_(std::numeric_limits<size_t>::max()),
      reentrantReturnSp_(nullptr), allocaTop_(0), singleStep_(false) {
    // Pre-allocate reasonable stack size
    valueStack_.resize(kMaxStackSize * kMaxCallDepth);
    valueStackStringOwned_.assign(valueStack_.size(), 0);
    callStack_.reserve(kMaxCallDepth);

    // Pre-allocate alloca buffer and reserve maximum capacity upfront.
    // The buffer MUST NOT reallocate during execution because alloca pointers
    // stored in registers and operand stack would become dangling.
    // Reserve the 16MB maximum so resize() never triggers reallocation.
    allocaBuffer_.reserve(16 * 1024 * 1024);
    allocaBuffer_.resize(64 * 1024);
}

/// @brief Tear down the VM: unwind execution state, release owned globals and
///        the trap record, and unref every cached rt_string literal.
BytecodeVM::~BytecodeVM() {
    resetExecutionState();
    releaseOwnedGlobals();
    clearTrapRecord();
    // Release all cached rt_string objects. Teardown is allowed to observe an
    // already-invalid handle after earlier unwinding, so only unref live
    // registry entries and always clear the slot.
    for (rt_string &s : stringCache_) {
        if (s && rt_string_is_handle(s))
            rt_string_unref(s);
        s = nullptr;
    }
    stringCache_.clear();
}

/// @brief Reset the string cache slots for the loaded module.
/// @details Releases any live cached handles before sizing the cache to the
///          current module's string pool. The runtime expects managed
///          `rt_string` handles, not raw C strings; handles are materialized
///          lazily by @ref getStringLiteral so worker VMs only create literals
///          they actually execute.
void BytecodeVM::initStringCache() {
    // Release any existing cache
    for (rt_string &s : stringCache_) {
        if (s && rt_string_is_handle(s))
            rt_string_unref(s);
        s = nullptr;
    }
    stringCache_.clear();

    if (!module_)
        return;

    stringCache_.assign(module_->stringPool.size(), nullptr);
}

/// @brief Lazily materialize and cache the rt_string for string-pool entry
///        @p idx; returns NULL if there is no module or @p idx is out of range.
/// @details Handles are created on first use so a worker VM only allocates the
///          literals it actually executes.
/// @param idx Zero-based module string-pool index.
/// @return Borrowed cached runtime handle, or null for an invalid index.
rt_string BytecodeVM::getStringLiteral(uint16_t idx) {
    if (!module_ || idx >= module_->stringPool.size())
        return nullptr;
    if (idx >= stringCache_.size())
        stringCache_.resize(module_->stringPool.size(), nullptr);

    rt_string &cached = stringCache_[idx];
    if (!cached)
        cached = rt_const_cstr(module_->stringPool[idx].c_str());
    return cached;
}

/// @brief True if runtime function @p name takes ownership of string args via
///        a *clone* (caller keeps its reference). Must mirror rtgen's
///        needsConsumingStringHandler() — e.g. rt_str_concat.
/// @param name Runtime registry name to classify.
/// @return `true` when the bridge must pass retained clones of string arguments.
bool BytecodeVM::runtimeCallConsumesClonedStringArgs(std::string_view name) {
    // Keep this list aligned with rtgen's needsConsumingStringHandler().
    return name == "rt_str_concat" || name == "Zanna.String.Concat";
}

/// @brief True if runtime function @p name consumes the *caller's owned*
///        string reference (e.g. rt_str_release_maybe) — the slot's ownership
///        flag must be cleared after the call.
/// @param name Runtime registry name to classify.
/// @return `true` when ownership transfers directly from VM argument slots.
bool BytecodeVM::runtimeCallConsumesOwnedStringArgs(std::string_view name) {
    return name == "rt_str_release_maybe";
}

/// @brief For a clone-consuming runtime call, return a copy of @p args with an
///        extra retain on each string parameter, so the callee can consume its
///        reference while the VM's originals stay valid. Empty if N/A.
/// @param ref Native-call descriptor and ownership metadata.
/// @param args Borrowed contiguous argument slots.
/// @param argCount Number of slots available at @p args.
/// @return Retained argument copy, or an empty vector when cloning is unnecessary.
std::vector<BCSlot> BytecodeVM::cloneRuntimeStringArgs(const NativeFuncRef &ref,
                                                       const BCSlot *args,
                                                       size_t argCount) const {
    if (!args || argCount == 0)
        return {};
    if (!ref.consumesClonedStringArgs && !runtimeCallConsumesClonedStringArgs(ref.name))
        return {};

    const auto *sig = ref.runtimeSignature;
    if (!sig)
        return {};

    std::vector<BCSlot> cloned(args, args + argCount);
    for (size_t i = 0; i < argCount; ++i) {
        if (runtimeParamIsString(sig, i))
            rt_str_retain_maybe(static_cast<rt_string>(cloned[i].ptr));
    }
    return cloned;
}

/// @brief Drop the extra retains added by @ref cloneRuntimeStringArgs once the
///        runtime call has returned (balances the clone's reference counts).
/// @param ref Native-call descriptor used to identify string parameters.
/// @param args Mutable cloned slots whose extra references are released.
void BytecodeVM::releaseRuntimeStringArgs(const NativeFuncRef &ref,
                                          std::vector<BCSlot> &args) const {
    if (args.empty())
        return;
    if (!ref.consumesClonedStringArgs && !runtimeCallConsumesClonedStringArgs(ref.name))
        return;

    const auto *sig = ref.runtimeSignature;
    if (!sig)
        return;
    for (size_t i = 0; i < args.size(); ++i) {
        if (runtimeParamIsString(sig, i))
            rt_str_release_maybe(static_cast<rt_string>(args[i].ptr));
    }
}

/// @brief Call a native runtime function through the VM RuntimeBridge.
/// @details Clones string args when the callee consumes them, reinterprets
///          BCSlots as VM Slots, and dispatches by descriptor or by name. A
///          RuntimeTrapSignal is converted to a VM trap (via dispatchTrap, or
///          a hard trap if unhandled) and the call reports failure.
/// @param ref Native runtime function and signature metadata.
/// @param args Borrowed VM argument slots; string ownership remains with the VM.
/// @param argCount Number of argument slots.
/// @param result Receives the runtime return bits on success.
/// @return true on normal return (@p result set); false if the call trapped.
bool BytecodeVM::invokeRuntimeBridgeNative(const NativeFuncRef &ref,
                                           BCSlot *args,
                                           uint8_t argCount,
                                           BCSlot &result) {
    std::vector<BCSlot> preservedArgs =
        cloneRuntimeStringArgs(ref, args, static_cast<size_t>(argCount));
    BCSlot *callArgs = preservedArgs.empty() ? args : preservedArgs.data();
    il::vm::Slot *vmArgs = reinterpret_cast<il::vm::Slot *>(callArgs);
    std::span<const il::vm::Slot> argSpan{vmArgs, static_cast<size_t>(argCount)};

    il::vm::RuntimeCallContext ctx;
    try {
        il::vm::ScopedRuntimeTrapInterceptor trapInterceptor(&bytecodeRuntimeTrapPassthrough, this);
        il::vm::Slot vmResult =
            ref.runtimeDescriptor
                ? il::vm::RuntimeBridge::call(ctx,
                                              *ref.runtimeDescriptor,
                                              argSpan,
                                              il::support::SourceLoc{},
                                              fp_ && fp_->func ? fp_->func->name : std::string{},
                                              "")
                : il::vm::RuntimeBridge::call(ctx,
                                              ref.name,
                                              argSpan,
                                              il::support::SourceLoc{},
                                              fp_ && fp_->func ? fp_->func->name : std::string{},
                                              "");
        result.i64 = vmResult.i64;
    } catch (const il::vm::RuntimeTrapSignal &signal) {
        releaseRuntimeStringArgs(ref, preservedArgs);
        if (!dispatchTrap(static_cast<TrapKind>(signal.kind), signal.code, signal.message.c_str()))
            trap(static_cast<TrapKind>(signal.kind), signal.message.c_str());
        return false;
    }

    releaseRuntimeStringArgs(ref, preservedArgs);
    return true;
}

/// @brief After an owned-consuming runtime call, clear the VM-side ownership
///        flag on each string arg so the VM won't double-release a handle the
///        callee already took ownership of.
/// @param ref Native-call descriptor used to identify consumed parameters.
/// @param args Borrowed argument slots whose ownership flags may be cleared.
/// @param argCount Number of argument slots.
void BytecodeVM::dismissConsumedStringArgs(const NativeFuncRef &ref,
                                           BCSlot *args,
                                           uint8_t argCount) {
    if (!args || argCount == 0)
        return;
    if (!ref.consumesOwnedStringArgs && !runtimeCallConsumesOwnedStringArgs(ref.name))
        return;

    const auto *sig = ref.runtimeSignature;
    if (!sig)
        return;

    for (uint8_t i = 0; i < argCount; ++i) {
        if (runtimeParamIsString(sig, i))
            setSlotOwnsString(args + i, false);
    }
}

/// @brief Register a C++ handler for native function @p name, overriding any
///        prior handler of that name (used for builtins not in the bridge).
/// @param name Exact runtime symbol used by bytecode native-call entries.
/// @param handler Callable stored by value in the VM handler table.
void BytecodeVM::registerNativeHandler(const std::string &name, NativeHandler handler) {
    nativeHandlers_[name] = std::move(handler);
}

/// @brief Snapshot the tunable execution settings (bridge enabled, dispatch
///        mode, trusted flag, native handler table) for transfer to a worker VM.
/// @return Self-contained copy of settings; it does not retain this VM.
BytecodeVM::ExecutionEnvironment BytecodeVM::captureExecutionEnvironment() const {
    ExecutionEnvironment env;
    env.runtimeBridgeEnabled = runtimeBridgeEnabled_;
    env.useThreadedDispatch = useThreadedDispatch_;
    env.trustedDispatch = trustedDispatch_;
    env.maxInstructions = maxInstrCount_;
    env.nativeHandlers = nativeHandlers_;
    return env;
}

/// @brief Apply a previously captured execution environment onto this VM.
/// @param env Settings and native handlers to copy.
/// @details Trusted dispatch is enabled only if both @p env requests it and the
///          currently loaded module passed structural validation.
void BytecodeVM::applyExecutionEnvironment(const ExecutionEnvironment &env) {
    runtimeBridgeEnabled_ = env.runtimeBridgeEnabled;
    useThreadedDispatch_ = env.useThreadedDispatch;
    trustedDispatchRequested_ = env.trustedDispatch;
    trustedDispatch_ = trustedDispatchRequested_ && moduleDispatchValidated_;
    maxInstrCount_ = env.maxInstructions;
    nativeHandlers_ = env.nativeHandlers;
}

/// @brief Convenience: copy @p other's execution environment onto this VM
///        (used when a spawned worker VM should mirror its parent's config).
/// @param other Source VM; module and transient execution state are not copied.
void BytecodeVM::copyExecutionEnvironmentFrom(const BytecodeVM &other) {
    applyExecutionEnvironment(other.captureExecutionEnvironment());
}

/// @brief Validate module header, lookup tables, function metadata, and bytecode.
/// @details This is stronger than the hot-path interpreter guards: it walks
///          every instruction, validates inline-word payloads, verifies branch
///          targets land on instruction boundaries, and checks side-table
///          indices before trusted dispatch can be enabled.
/// @param module Candidate module; may be null.
/// @param failure Receives the first trap kind and explanatory message.
/// @return `true` only when every module-level and function-level invariant holds.
bool BytecodeVM::validateModuleForLoad(const BytecodeModule *module,
                                       ModuleValidationFailure &failure) const {
    /// @brief Store the first module-validation failure.
    /// @param kind Trap classification for the failure.
    /// @param message Explanatory diagnostic.
    /// @return Always `false`.
    auto fail = [&failure](TrapKind kind, std::string message) {
        failure.kind = kind;
        failure.message = std::move(message);
        return false;
    };

    if (!module)
        return fail(TrapKind::RuntimeError, "No bytecode module supplied");
    if (module->magic != kBytecodeModuleMagic)
        return fail(TrapKind::InvalidOpcode, "Invalid bytecode module magic");
    if (module->version != kBytecodeVersion)
        return fail(TrapKind::InvalidOpcode, "Unsupported bytecode module version");
    if (module->functions.size() > std::numeric_limits<uint32_t>::max())
        return fail(TrapKind::InvalidOpcode, "Function table exceeds bytecode index width");
    if (module->nativeFuncs.size() > std::numeric_limits<uint16_t>::max())
        return fail(TrapKind::InvalidOpcode, "Native function table exceeds bytecode index width");
    if (module->globals.size() > std::numeric_limits<uint16_t>::max())
        return fail(TrapKind::InvalidOpcode, "Global table exceeds bytecode index width");
    if (module->i64Pool.size() > std::numeric_limits<uint16_t>::max() ||
        module->f64Pool.size() > std::numeric_limits<uint16_t>::max() ||
        module->stringPool.size() > std::numeric_limits<uint16_t>::max()) {
        return fail(TrapKind::InvalidOpcode, "Constant pool exceeds bytecode index width");
    }

    for (const auto &entry : module->functionIndex) {
        if (entry.second >= module->functions.size())
            return fail(TrapKind::InvalidOpcode, "Function index table out of range");
        if (module->functions[entry.second].name != entry.first)
            return fail(TrapKind::InvalidOpcode, "Function index table name mismatch");
    }
    for (size_t i = 0; i < module->functions.size(); ++i) {
        const auto &fn = module->functions[i];
        auto it = module->functionIndex.find(fn.name);
        if (fn.name.empty())
            return fail(TrapKind::InvalidOpcode, "Function name must not be empty");
        if (it == module->functionIndex.end() || it->second != i)
            return fail(TrapKind::InvalidOpcode, "Function table missing name index entry");
    }

    for (const auto &entry : module->nativeFuncIndex) {
        if (entry.second >= module->nativeFuncs.size())
            return fail(TrapKind::InvalidOpcode, "Native function index table out of range");
    }
    for (size_t i = 0; i < module->nativeFuncs.size(); ++i) {
        const NativeFuncRef &ref = module->nativeFuncs[i];
        if (ref.name.empty())
            return fail(TrapKind::InvalidOpcode, "Native function name must not be empty");
        if (ref.paramCount > std::numeric_limits<uint8_t>::max())
            return fail(TrapKind::InvalidOpcode, "Native function arity exceeds bytecode width");
        const std::string key = detail::nativeFunctionKey(ref.name, ref.paramCount, ref.hasReturn);
        auto found = module->nativeFuncIndex.find(key);
        if (found == module->nativeFuncIndex.end() || found->second != i)
            return fail(TrapKind::InvalidOpcode, "Native function table missing signature key");
    }

    for (const auto &entry : module->globalIndex) {
        if (entry.second >= module->globals.size())
            return fail(TrapKind::InvalidOpcode, "Global index table out of range");
        if (module->globals[entry.second].name != entry.first)
            return fail(TrapKind::InvalidOpcode, "Global index table name mismatch");
    }
    for (size_t i = 0; i < module->globals.size(); ++i) {
        const GlobalInfo &global = module->globals[i];
        auto found = module->globalIndex.find(global.name);
        if (global.name.empty())
            return fail(TrapKind::InvalidOpcode, "Global name must not be empty");
        if (found == module->globalIndex.end() || found->second != i)
            return fail(TrapKind::InvalidOpcode, "Global table missing name index entry");
        if (global.initData.size() > sizeof(BCSlot))
            return fail(TrapKind::InvalidOpcode, "Global initializer exceeds slot width");
    }

    for (size_t i = 0; i < module->functions.size(); ++i) {
        if (!validateFunctionForLoad(*module, module->functions[i], i, failure))
            return false;
    }
    return true;
}

/// @brief Validate one bytecode function before load() publishes the module.
/// @details The scan records all instruction-start PCs, validates operands
///          that index module tables, checks variable-length instruction
///          payloads, then verifies every branch/EH/switch target points to a
///          valid instruction boundary rather than into an inline data word.
/// @param module Module that owns @p func and its referenced tables.
/// @param func Function whose code and metadata are validated.
/// @param functionIndex Expected position of @p func in the module table.
/// @param failure Receives the first validation error.
/// @return `true` when the function is safe for validated dispatch.
bool BytecodeVM::validateFunctionForLoad(const BytecodeModule &module,
                                         const BytecodeFunction &func,
                                         size_t functionIndex,
                                         ModuleValidationFailure &failure) const {
    /// @brief Store one invalid-bytecode diagnostic for the current function.
    /// @param message Function-specific validation failure.
    /// @return Always `false`.
    auto fail = [&failure, &func](std::string message) {
        failure.kind = TrapKind::InvalidOpcode;
        failure.message = "Invalid bytecode function @" + func.name + ": " + std::move(message);
        return false;
    };

    if (functionIndex > std::numeric_limits<uint32_t>::max())
        return fail("function index exceeds bytecode width");
    if (func.numLocals < func.numParams)
        return fail("parameter count exceeds local count");
    if (func.localIsString.size() > func.numLocals)
        return fail("string-local bitmap exceeds local count");
    if (func.code.empty())
        return fail("function has no bytecode");
    if (func.code.size() > std::numeric_limits<uint32_t>::max())
        return fail("code stream exceeds bytecode PC width");
    if (!func.lineTable.empty() && func.lineTable.size() != func.code.size())
        return fail("line table size does not match code size");
    if (!func.sourceFileTable.empty() && func.sourceFileTable.size() != func.code.size())
        return fail("source file table size does not match code size");
    if (!func.blockLabelTable.empty() && func.blockLabelTable.size() != func.code.size())
        return fail("block label table size does not match code size");
    if ((func.sourceFileIdx != 0 || !module.sourceFiles.empty()) &&
        func.sourceFileIdx >= module.sourceFiles.size()) {
        return fail("default source file index out of range");
    }
    for (uint32_t sourceEntry : func.sourceFileTable) {
        if (sourceEntry > module.sourceFiles.size())
            return fail("per-PC source file index out of range");
    }
    for (const LocalVarInfo &local : func.localVars) {
        if (local.localIdx >= func.numLocals)
            return fail("local variable debug metadata local index out of range");
        if (local.startPc > local.endPc || local.endPc > func.code.size())
            return fail("local variable debug metadata PC range out of range");
    }

    std::vector<uint8_t> instructionStarts(func.code.size(), 0);

    struct TargetCheck {
        uint32_t target = 0;
        const char *site = nullptr;
    };

    std::vector<TargetCheck> targets;

    /// @brief Resolve a signed relative bytecode target without overflow.
    /// @param basePc Base program counter.
    /// @param offset Signed relative displacement.
    /// @param[out] target Receives the absolute target on success.
    /// @return `true` when the absolute target fits in `uint32_t`.
    auto relativeTarget = [](uint32_t basePc, int32_t offset, uint32_t &target) {
        const int64_t absolute = static_cast<int64_t>(basePc) + static_cast<int64_t>(offset);
        if (absolute < 0 || absolute > std::numeric_limits<uint32_t>::max())
            return false;
        target = static_cast<uint32_t>(absolute);
        return true;
    };
    /// @brief Queue one absolute control-flow target for boundary validation.
    /// @param target Absolute bytecode program counter.
    /// @param site Static instruction-site description.
    auto addTarget = [&](uint32_t target, const char *site) {
        targets.push_back(TargetCheck{target, site});
    };
    /// @brief Resolve and queue one relative control-flow target.
    /// @param basePc Base program counter.
    /// @param offset Signed relative displacement.
    /// @param site Static instruction-site description.
    /// @return `false` when the target overflows the bytecode PC width.
    auto addRelativeTarget = [&](uint32_t basePc, int32_t offset, const char *site) {
        uint32_t target = 0;
        if (!relativeTarget(basePc, offset, target))
            return false;
        addTarget(target, site);
        return true;
    };
    /// @brief Validate one local-variable index.
    /// @param idx Local index to inspect.
    /// @param site Static instruction-site description.
    /// @return `true` when `idx` lies within the function's local table.
    auto requireLocal = [&](uint32_t idx, const char *site) {
        if (idx >= func.numLocals)
            return fail(std::string(site) + " local index out of range");
        return true;
    };
    /// @brief Validate one encoded integer-width argument.
    /// @param encoded Encoded width value.
    /// @param site Static instruction-site description.
    /// @return `true` when the width argument is supported.
    auto requireWidthArg = [&](uint8_t encoded, const char *site) {
        if (!detail::isValidWidthArg(encoded))
            return fail(std::string(site) + " width argument out of range");
        return true;
    };

    for (uint32_t pc = 0; pc < func.code.size();) {
        instructionStarts[pc] = 1;
        const uint32_t instr = func.code[pc];
        const uint8_t rawOpcode = static_cast<uint8_t>(instr & 0xFFu);
        if (!isKnownOpcode(rawOpcode))
            return fail("unknown opcode byte at pc " + std::to_string(pc));

        const BCOpcode op = decodeOpcode(instr);
        switch (op) {
            case BCOpcode::OPCODE_COUNT:
            case BCOpcode::TAIL_CALL:
            case BCOpcode::MAKE_ERROR:
                return fail(std::string(opcodeName(op)) + " is not executable bytecode");

            case BCOpcode::LOAD_I32:
                if (pc + 1 >= func.code.size())
                    return fail("LOAD_I32 missing inline value word");
                pc += 2;
                continue;

            case BCOpcode::LOAD_I64:
                if (decodeArg16(instr) >= module.i64Pool.size())
                    return fail("LOAD_I64 constant index out of range");
                break;
            case BCOpcode::LOAD_F64:
                if (decodeArg16(instr) >= module.f64Pool.size())
                    return fail("LOAD_F64 constant index out of range");
                break;
            case BCOpcode::LOAD_STR:
                if (decodeArg16(instr) >= module.stringPool.size())
                    return fail("LOAD_STR constant index out of range");
                break;

            case BCOpcode::LOAD_LOCAL:
            case BCOpcode::STORE_LOCAL:
            case BCOpcode::INC_LOCAL:
            case BCOpcode::DEC_LOCAL:
                if (!requireLocal(decodeArg8_0(instr), opcodeName(op)))
                    return false;
                break;
            case BCOpcode::LOAD_LOCAL_W:
            case BCOpcode::STORE_LOCAL_W:
                if (!requireLocal(decodeArg16(instr), opcodeName(op)))
                    return false;
                break;

            case BCOpcode::LOAD_GLOBAL:
            case BCOpcode::STORE_GLOBAL:
            case BCOpcode::LOAD_GLOBAL_ADDR:
                if (decodeArg16(instr) >= module.globals.size())
                    return fail(std::string(opcodeName(op)) + " global index out of range");
                break;

            case BCOpcode::CALL:
                if (decodeArg16(instr) >= module.functions.size())
                    return fail("CALL function index out of range");
                break;
            case BCOpcode::CALL_NATIVE: {
                const uint8_t argCount = decodeArg8_0(instr);
                const uint16_t nativeIdx = decodeArg16_1(instr);
                if (nativeIdx >= module.nativeFuncs.size())
                    return fail("CALL_NATIVE native index out of range");
                if (argCount != module.nativeFuncs[nativeIdx].paramCount)
                    return fail("CALL_NATIVE encoded arity mismatch");
                break;
            }

            case BCOpcode::ADD_I64_OVF:
            case BCOpcode::SUB_I64_OVF:
            case BCOpcode::MUL_I64_OVF:
            case BCOpcode::SDIV_I64_CHK:
            case BCOpcode::UDIV_I64_CHK:
            case BCOpcode::SREM_I64_CHK:
            case BCOpcode::UREM_I64_CHK:
            case BCOpcode::IDX_CHK:
            case BCOpcode::F64_TO_I64_CHK:
            case BCOpcode::F64_TO_U64_CHK:
            case BCOpcode::I64_NARROW_CHK:
            case BCOpcode::U64_NARROW_CHK:
                if (!requireWidthArg(decodeArg8_0(instr), opcodeName(op)))
                    return false;
                break;

            case BCOpcode::JUMP:
            case BCOpcode::JUMP_IF_TRUE:
            case BCOpcode::JUMP_IF_FALSE:
                if (!addRelativeTarget(pc + 1, decodeArgI16(instr), opcodeName(op)))
                    return fail(std::string(opcodeName(op)) + " target under/overflows PC range");
                break;
            case BCOpcode::JUMP_LONG:
                if (!addRelativeTarget(pc + 1, decodeArgI24(instr), "JUMP_LONG"))
                    return fail("JUMP_LONG target under/overflows PC range");
                break;

            case BCOpcode::EH_PUSH: {
                if (pc + 1 >= func.code.size())
                    return fail("EH_PUSH missing handler offset word");
                if (!addRelativeTarget(
                        pc + 1, static_cast<int32_t>(func.code[pc + 1]), "EH_PUSH")) {
                    return fail("EH_PUSH handler target under/overflows PC range");
                }
                pc += 2;
                continue;
            }

            case BCOpcode::RESUME_LABEL: {
                if (pc + 1 >= func.code.size())
                    return fail("RESUME_LABEL missing target offset word");
                if (!addRelativeTarget(
                        pc + 1, static_cast<int32_t>(func.code[pc + 1]), "RESUME_LABEL")) {
                    return fail("RESUME_LABEL target under/overflows PC range");
                }
                pc += 2;
                continue;
            }

            case BCOpcode::SWITCH: {
                if (pc + 2 >= func.code.size())
                    return fail("SWITCH missing header words");
                const uint32_t numCases = func.code[pc + 1];
                const uint64_t caseWords = static_cast<uint64_t>(numCases) * 2u;
                const uint64_t endPc = static_cast<uint64_t>(pc) + 3u + caseWords;
                if (endPc > func.code.size())
                    return fail("SWITCH case table extends past function code");
                if (!addRelativeTarget(
                        pc + 2, static_cast<int32_t>(func.code[pc + 2]), "SWITCH default")) {
                    return fail("SWITCH default target under/overflows PC range");
                }
                std::unordered_set<int32_t> seenCases;
                uint32_t cursor = pc + 3;
                for (uint32_t i = 0; i < numCases; ++i) {
                    const int32_t caseValue = static_cast<int32_t>(func.code[cursor++]);
                    if (!seenCases.insert(caseValue).second)
                        return fail("SWITCH contains duplicate case value");
                    const uint32_t offsetPc = cursor++;
                    if (!addRelativeTarget(
                            offsetPc, static_cast<int32_t>(func.code[offsetPc]), "SWITCH case")) {
                        return fail("SWITCH case target under/overflows PC range");
                    }
                }
                pc = static_cast<uint32_t>(endPc);
                continue;
            }

            default:
                break;
        }
        ++pc;
    }

    for (const TargetCheck &target : targets) {
        if (target.target >= instructionStarts.size() || instructionStarts[target.target] == 0) {
            return fail(std::string(target.site) +
                        " target does not land on an instruction boundary");
        }
    }

    for (const ExceptionRange &range : func.exceptionRanges) {
        if (range.startPc > range.endPc || range.endPc > func.code.size() ||
            range.handlerPc >= instructionStarts.size() ||
            instructionStarts[range.handlerPc] == 0) {
            return fail("exception range metadata is out of range");
        }
        if (range.startPc < instructionStarts.size() && instructionStarts[range.startPc] == 0)
            return fail("exception range start is not an instruction boundary");
        if (range.endPc < instructionStarts.size() && instructionStarts[range.endPc] == 0)
            return fail("exception range end is not an instruction boundary");
    }
    for (const SwitchTable &table : func.switchTables) {
        if (table.defaultPc >= instructionStarts.size() || instructionStarts[table.defaultPc] == 0)
            return fail("switch metadata default target is out of range");
        std::unordered_set<int64_t> seenCases;
        for (const SwitchEntry &entry : table.entries) {
            if (!seenCases.insert(entry.value).second)
                return fail("switch metadata contains duplicate case value");
            if (entry.targetPc >= instructionStarts.size() ||
                instructionStarts[entry.targetPc] == 0) {
                return fail("switch metadata case target is out of range");
            }
        }
    }

    return true;
}

/// @brief Check whether a function pointer belongs to the currently loaded module.
/// @param func Candidate function address.
/// @return `true` when @p func exactly identifies an element of the loaded table.
bool BytecodeVM::functionBelongsToModule(const BytecodeFunction *func) const {
    if (!module_ || !func)
        return false;
    for (const BytecodeFunction &candidate : module_->functions) {
        if (&candidate == func)
            return true;
    }
    return false;
}

/// @brief Bind @p module for execution: install unified runtime handlers,
///        reset state, and allocate + initialize global storage (string
///        globals get an owned rt_string from their init bytes; scalar
///        globals are memcpy'd from their baked init image) plus the string
///        literal cache. Safe to call again to re-load a different module.
/// @param module Borrowed module that must outlive every synchronous execution.
/// @post On validation failure the VM is trapped and no module is published.
void BytecodeVM::load(const BytecodeModule *module) {
    registerUnifiedVmRuntimeHandlers();
    resetExecutionState();
    releaseOwnedGlobals();
    clearTrapRecord();

    module_ = nullptr;
    loadFailed_ = false;
    moduleDispatchValidated_ = false;
    trustedDispatch_ = false;
    state_ = VMState::Ready;
    trapKind_ = TrapKind::None;
    currentErrorCode_ = 0;
    pendingTrapErrorCode_ = false;
    trapMessage_.clear();

    ModuleValidationFailure validationFailure;
    if (!validateModuleForLoad(module, validationFailure)) {
        loadFailed_ = true;
        trap(validationFailure.kind, validationFailure.message.c_str());
        return;
    }

    module_ = module;
    moduleDispatchValidated_ = true;
    trustedDispatch_ = trustedDispatchRequested_;
    loadFailed_ = false;

    // Initialize global variable storage
    globals_.clear();
    globalsStringOwned_.clear();
    if (module_) {
        globals_.resize(module_->globals.size());
        globalsStringOwned_.assign(module_->globals.size(), 0);
        for (size_t i = 0; i < module_->globals.size(); ++i) {
            globals_[i].i64 = 0; // Zero-initialize
            const auto &gi = module_->globals[i];
            if (gi.type.kind == il::core::Type::Kind::Str) {
                globals_[i].ptr = rt_string_from_bytes(gi.initString.data(), gi.initString.size());
                globalsStringOwned_[i] = globals_[i].ptr ? 1 : 0;
            } else if (!gi.initData.empty()) {
                size_t copySize = std::min<size_t>(gi.initData.size(), sizeof(BCSlot));
                std::memcpy(&globals_[i], gi.initData.data(), copySize);
            }
        }
    }

    // Initialize string cache with proper rt_string objects
    initStringCache();
}

/// @brief Index of @p slot within the value stack (the key into the parallel
///        string-ownership bitmap). Asserts the pointer is in range.
/// @param slot Address of a slot in `valueStack_`.
/// @return Zero-based slot index.
size_t BytecodeVM::slotIndex(const BCSlot *slot) const {
    assert(slot >= valueStack_.data());
    assert(slot < valueStack_.data() + valueStack_.size());
    return static_cast<size_t>(slot - valueStack_.data());
}

/// @brief True if @p slot currently holds an owned string reference (the VM
///        is responsible for releasing it).
/// @param slot Address of a slot in `valueStack_`.
/// @return Ownership bit associated with @p slot.
bool BytecodeVM::slotOwnsString(const BCSlot *slot) const {
    return valueStackStringOwned_.owns(slotIndex(slot));
}

/// @brief Set/clear @p slot's "owns string reference" flag.
/// @param slot Address of a slot in `valueStack_`.
/// @param owns New ownership state.
void BytecodeVM::setSlotOwnsString(const BCSlot *slot, bool owns) {
    valueStackStringOwned_.set(slotIndex(slot), owns);
}

/// @brief True if local slot @p idx of @p frame is typed as a string (so its
///        contents are reference-counted).
/// @param frame Frame whose local-type bitmap is consulted.
/// @param idx Zero-based local index.
/// @return `true` when the local exists in the bitmap and is string-typed.
bool BytecodeVM::localIsString(const BCFrame &frame, uint32_t idx) const {
    return idx < frame.func->localIsString.size() && frame.func->localIsString[idx] != 0;
}

/// @brief Defensive check that @p ptr is a live rt_string handle.
/// @return true if NULL or a valid handle; otherwise traps (RuntimeError,
///         naming @p site) and returns false. Guards against type-confused
///         slots corrupting the runtime string heap.
/// @param ptr Candidate runtime string handle.
/// @param site Operation name included in a generated trap.
bool BytecodeVM::validateStringHandle(const void *ptr, const char *site) {
    if (!ptr || rt_string_is_handle(ptr))
        return true;

    const char *functionName = (fp_ && fp_->func) ? fp_->func->name.c_str() : "<none>";
    const uint32_t pc = fp_ ? fp_->pc : 0;
    trap(TrapKind::RuntimeError,
         (std::string(site) + ": invalid runtime string handle in " + functionName +
          " @pc=" + std::to_string(pc))
             .c_str());
    return false;
}

/// @brief If @p slot owns a string, release that reference and clear both the
///        pointer and the ownership flag (idempotent; tolerates a bad handle
///        by just dropping it).
/// @param slot Mutable value-stack slot.
void BytecodeVM::releaseOwnedString(BCSlot *slot) {
    if (!slotOwnsString(slot))
        return;
    if (!validateStringHandle(slot->ptr, "BytecodeVM::releaseOwnedString")) {
        slot->ptr = nullptr;
        setSlotOwnsString(slot, false);
        return;
    }
    rt_str_release_maybe(static_cast<rt_string>(slot->ptr));
    slot->ptr = nullptr;
    setSlotOwnsString(slot, false);
}

/// @brief Retain @p slot's runtime string handle and mark the slot as owning it.
/// @details Null slots are treated as a successful no-op with ownership cleared.
///          Non-null slots are validated before the reference count is incremented
///          so type-confused stack data traps instead of corrupting the runtime
///          string heap.
/// @param slot Mutable value-stack slot to retain and mark owning.
/// @param site Operation name included in a validation trap.
/// @return `true` on success; `false` after trapping on an invalid handle.
bool BytecodeVM::retainStringSlot(BCSlot *slot, const char *site) {
    if (!slot->ptr) {
        setSlotOwnsString(slot, false);
        return true;
    }
    if (!validateStringHandle(slot->ptr, site))
        return false;
    rt_str_retain_maybe(static_cast<rt_string>(slot->ptr));
    setSlotOwnsString(slot, true);
    return true;
}

/// @brief Copy @p src into @p dst and duplicate owned string lifetime if needed.
/// @details The destination is first marked non-owning so any early trap cannot
///          accidentally release an alias. When @p src owns a non-null runtime
///          string handle, the copied handle is validated and retained before
///          @p dst becomes owning.
/// @param dst Destination value-stack slot.
/// @param src Source value-stack slot.
/// @param site Operation name included in a validation trap.
/// @return `true` when the copy and any required retain succeeded.
bool BytecodeVM::copyStackSlotRetainingString(BCSlot *dst, const BCSlot *src, const char *site) {
    *dst = *src;
    setSlotOwnsString(dst, false);
    if (!slotOwnsString(src) || !dst->ptr)
        return true;
    if (!validateStringHandle(dst->ptr, site)) {
        dst->ptr = nullptr;
        return false;
    }
    rt_str_retain_maybe(static_cast<rt_string>(dst->ptr));
    setSlotOwnsString(dst, true);
    return true;
}

/// @brief Duplicate the top operand stack slot, preserving string ownership.
/// @param site Operation name included in any validation trap.
/// @return `true` when the duplicate was produced.
bool BytecodeVM::duplicateTopSlot(const char *site) {
    if (!copyStackSlotRetainingString(sp_, sp_ - 1, site))
        return false;
    ++sp_;
    return true;
}

/// @brief Duplicate the top two operand stack slots, preserving string ownership.
/// @param site Operation name included in any validation trap.
/// @return `true` when both duplicates were produced.
bool BytecodeVM::duplicateTopTwoSlots(const char *site) {
    BCSlot *dst0 = sp_;
    BCSlot *dst1 = sp_ + 1;
    const BCSlot *src0 = sp_ - 2;
    const BCSlot *src1 = sp_ - 1;
    if (!copyStackSlotRetainingString(dst0, src0, site))
        return false;
    if (!copyStackSlotRetainingString(dst1, src1, site)) {
        releaseOwnedString(dst0);
        return false;
    }
    sp_ += 2;
    return true;
}

/// @brief Release @p count owned operand stack slots and move the stack pointer down.
/// @param count Number of topmost slots to destroy.
void BytecodeVM::popOwnedSlots(size_t count) {
    for (size_t i = 0; i < count; ++i)
        releaseOwnedString(--sp_);
}

/// @brief Swap the top two operand slots together with their ownership flags.
void BytecodeVM::swapTopTwoSlots() {
    BCSlot tmp = sp_[-1];
    const bool topOwns = slotOwnsString(sp_ - 1);
    const bool lowerOwns = slotOwnsString(sp_ - 2);
    sp_[-1] = sp_[-2];
    sp_[-2] = tmp;
    setSlotOwnsString(sp_ - 1, lowerOwns);
    setSlotOwnsString(sp_ - 2, topOwns);
}

/// @brief Rotate the top three operand slots together with their ownership flags.
void BytecodeVM::rotateTopThreeSlots() {
    BCSlot tmp = sp_[-1];
    const bool topOwns = slotOwnsString(sp_ - 1);
    const bool secondOwns = slotOwnsString(sp_ - 2);
    const bool firstOwns = slotOwnsString(sp_ - 3);
    sp_[-1] = sp_[-2];
    sp_[-2] = sp_[-3];
    sp_[-3] = tmp;
    setSlotOwnsString(sp_ - 1, secondOwns);
    setSlotOwnsString(sp_ - 2, firstOwns);
    setSlotOwnsString(sp_ - 3, topOwns);
}

/// @brief Push local slot @p idx onto the operand stack; if the local is a
///        string, retain the reference and mark the new slot as owning it.
/// @param idx Local index in the current frame.
/// @param site Operation name included in any trap.
/// @return `true` when the local was pushed successfully.
bool BytecodeVM::pushLocal(uint32_t idx, const char *site) {
    BCSlot *dst = sp_++;
    *dst = fp_->locals[idx];
    if (localIsString(*fp_, idx) && dst->ptr) {
        if (!validateStringHandle(dst->ptr, site)) {
            sp_--;
            return false;
        }
        rt_str_retain_maybe(static_cast<rt_string>(dst->ptr));
        setSlotOwnsString(dst, true);
    } else {
        setSlotOwnsString(dst, false);
    }
    return true;
}

/// @brief Pop the operand stack into local slot @p idx. For string locals,
///        releases the old value, then transfers the stack slot's reference
///        (retaining only if the source did not already own it) so the net
///        reference count stays balanced.
/// @param idx Local index in the current frame.
/// @param site Operation name included in any trap.
/// @return `true` when the value and ownership were stored successfully.
bool BytecodeVM::storeLocal(uint32_t idx, const char *site) {
    BCSlot *src = --sp_;
    BCSlot *dst = fp_->locals + idx;
    const bool srcOwns = slotOwnsString(src);
    const BCSlot value = *src;

    if (localIsString(*fp_, idx)) {
        releaseOwnedString(dst);
        *dst = value;
        if (value.ptr) {
            if (!validateStringHandle(value.ptr, site)) {
                setSlotOwnsString(src, false);
                setSlotOwnsString(dst, false);
                return false;
            }
            if (!srcOwns)
                rt_str_retain_maybe(static_cast<rt_string>(value.ptr));
            setSlotOwnsString(dst, true);
        } else {
            setSlotOwnsString(dst, false);
        }
    } else {
        *dst = value;
        setSlotOwnsString(dst, false);
    }

    setSlotOwnsString(src, false);
    return true;
}

/// @brief Pop a return value, unwind the current frame, and push the value to the caller.
/// @return `true` when execution should continue or halt normally; `false` on trap.
bool BytecodeVM::returnValueFromFrame() {
    BCSlot *resultSlot = --sp_;
    BCSlot result = *resultSlot;
    const bool resultOwnsString = slotOwnsString(resultSlot);
    setSlotOwnsString(resultSlot, false);
    if (!popFrame()) {
        *sp_++ = result;
        setSlotOwnsString(sp_ - 1, resultOwnsString);
        state_ = VMState::Halted;
        return false;
    }
    *sp_++ = result;
    setSlotOwnsString(sp_ - 1, resultOwnsString);
    if (reentrantStopDepth_ != std::numeric_limits<size_t>::max() &&
        callStack_.size() == reentrantStopDepth_) {
        BCSlot *restoreSp = reentrantReturnSp_;
        if (restoreSp) {
            setSlotOwnsString(sp_ - 1, false);
            sp_ = restoreSp;
            *sp_++ = result;
            setSlotOwnsString(sp_ - 1, resultOwnsString);
        }
        state_ = VMState::Halted;
        return false;
    }
    return true;
}

/// @brief Unwind a void-returning frame and halt when it was the entry frame.
/// @return `true` when execution should continue or halt normally; `false` on trap.
bool BytecodeVM::returnVoidFromFrame() {
    if (!popFrame()) {
        state_ = VMState::Halted;
        return false;
    }
    if (reentrantStopDepth_ != std::numeric_limits<size_t>::max() &&
        callStack_.size() == reentrantStopDepth_) {
        if (reentrantReturnSp_)
            sp_ = reentrantReturnSp_;
        state_ = VMState::Halted;
        return false;
    }
    return true;
}

/// @brief Push global slot @p idx onto the operand stack with string retain semantics.
/// @param idx Global-table index.
/// @param site Operation name included in any trap.
/// @return `true` when the global was pushed successfully.
bool BytecodeVM::loadGlobal(uint16_t idx, const char *site) {
    if (idx >= globals_.size()) {
        trap(TrapKind::InvalidOpcode, "LOAD_GLOBAL index out of range");
        return false;
    }

    *sp_++ = globals_[idx];
    if (idx < globalsStringOwned_.size() && globalsStringOwned_[idx] && globals_[idx].ptr) {
        if (!validateStringHandle(globals_[idx].ptr, site)) {
            --sp_;
            setSlotOwnsString(sp_, false);
            return false;
        }
        rt_str_retain_maybe(static_cast<rt_string>(globals_[idx].ptr));
        setSlotOwnsString(sp_ - 1, true);
    } else {
        setSlotOwnsString(sp_ - 1, false);
    }
    return true;
}

/// @brief Pop the operand stack into global slot @p idx, transferring string ownership.
/// @param idx Global-table index.
/// @param site Operation name included in any trap.
/// @return `true` when the value was stored successfully.
bool BytecodeVM::storeGlobal(uint16_t idx, const char *site) {
    BCSlot *src = --sp_;
    BCSlot value = *src;
    const bool valueOwnsString = slotOwnsString(src);

    if (idx >= globals_.size()) {
        releaseOwnedString(src);
        trap(TrapKind::InvalidOpcode, "STORE_GLOBAL index out of range");
        return false;
    }

    if (idx < globalsStringOwned_.size() && globalsStringOwned_[idx] && globals_[idx].ptr) {
        if (!validateStringHandle(globals_[idx].ptr, site)) {
            releaseOwnedString(src);
            return false;
        }
        rt_str_release_maybe(static_cast<rt_string>(globals_[idx].ptr));
    }

    globals_[idx] = value;
    if (idx < globalsStringOwned_.size())
        globalsStringOwned_[idx] = valueOwnsString ? 1 : 0;
    setSlotOwnsString(src, false);
    return true;
}

/// @brief Release any owned string references held in a call's argument slots.
/// @param args First argument slot.
/// @param argCount Number of consecutive slots to release.
void BytecodeVM::releaseCallArgs(BCSlot *args, uint8_t argCount) {
    for (uint8_t i = 0; i < argCount; ++i)
        releaseOwnedString(args + i);
}

/// @brief Release owned string references in every local of @p frame and clear
///        the ownership flags — called when a frame is torn down.
/// @param frame Frame whose local slots are being destroyed.
void BytecodeVM::releaseFrameLocals(const BCFrame &frame) {
    for (uint32_t i = 0; i < frame.func->numLocals; ++i) {
        if (localIsString(frame, i))
            releaseOwnedString(frame.locals + i);
        else
            setSlotOwnsString(frame.locals + i, false);
    }
}

/// @brief Release every owned string still sitting on the value stack — used
///        during teardown/unwind so a trap can't leak operand-stack strings.
void BytecodeVM::releaseOwnedValueStack() {
    for (size_t i = 0; i < valueStack_.size(); ++i) {
        if (!valueStackStringOwned_.owns(i))
            continue;
        releaseOwnedString(valueStack_.data() + i);
    }
}

/// @brief Release every owned string global (module teardown / re-load).
void BytecodeVM::releaseOwnedGlobals() {
    for (size_t i = 0; i < globals_.size(); ++i) {
        releaseOwnedGlobalString(i);
    }
}

/// @brief Reverse-map a raw pointer to a global slot to its global index, or
///        SIZE_MAX if @p ptr is not the address of a global (lets GEP/store
///        opcodes detect writes that target a string global).
/// @param ptr Candidate address.
/// @return Matching global index, or `SIZE_MAX`.
size_t BytecodeVM::globalIndexForAddress(const void *ptr) const {
    if (!ptr)
        return SIZE_MAX;
    for (size_t i = 0; i < globals_.size(); ++i) {
        if (ptr == static_cast<const void *>(&globals_[i]))
            return i;
    }
    return SIZE_MAX;
}

/// @brief Release the string reference owned by global @p idx (no-op if the
///        global is not an owned string), clearing pointer and ownership flag.
/// @param idx Global-table index.
void BytecodeVM::releaseOwnedGlobalString(size_t idx) {
    if (idx >= globals_.size() || idx >= globalsStringOwned_.size() ||
        globalsStringOwned_[idx] == 0)
        return;
    if (globals_[idx].ptr && validateStringHandle(globals_[idx].ptr, "BytecodeVM::globals")) {
        rt_str_release_maybe(static_cast<rt_string>(globals_[idx].ptr));
    }
    globals_[idx].ptr = nullptr;
    globalsStringOwned_[idx] = 0;
}

/// @brief Return the first global slot overlapped by @p ptr plus @p bytes.
/// @details Raw memory stores can target an interior byte of a global BCSlot via
///          GEP. Ownership bookkeeping must therefore detect overlap, not only
///          exact slot-address equality.
/// @param ptr First byte of the candidate store range.
/// @param bytes Range width; zero is treated as one byte.
/// @return First overlapped global index, or `SIZE_MAX` when there is no overlap.
size_t BytecodeVM::globalIndexForAddressRange(const void *ptr, size_t bytes) const {
    if (!ptr || globals_.empty())
        return SIZE_MAX;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    const size_t width = bytes == 0 ? 1 : bytes;
    if (width > std::numeric_limits<uintptr_t>::max() - addr)
        return SIZE_MAX;
    const uintptr_t end = addr + width;
    for (size_t i = 0; i < globals_.size(); ++i) {
        const uintptr_t slotBegin = reinterpret_cast<uintptr_t>(&globals_[i]);
        const uintptr_t slotEnd = slotBegin + sizeof(BCSlot);
        if (addr < slotEnd && end > slotBegin)
            return i;
    }
    return SIZE_MAX;
}

/// @brief Before a raw (untyped) store through a pointer that aliases a string
///        global, release the global's old reference so it isn't leaked.
/// @param ptr First byte of the store destination.
/// @param bytes Store width.
void BytecodeVM::clearGlobalStringOwnershipForRawStore(void *ptr, size_t bytes) {
    const size_t idx = globalIndexForAddressRange(ptr, bytes);
    if (idx == SIZE_MAX)
        return;
    releaseOwnedGlobalString(idx);
}

/// @brief Mark the string global aliased by @p ptr as owned/not-owned after a
///        typed store transferred a reference into it.
/// @param ptr Exact global slot address.
/// @param owns New ownership state.
void BytecodeVM::setGlobalStringOwnershipForAddress(void *ptr, bool owns) {
    const size_t idx = globalIndexForAddress(ptr);
    if (idx == SIZE_MAX || idx >= globalsStringOwned_.size())
        return;
    globalsStringOwned_[idx] = owns ? 1 : 0;
}

/// @brief Release any owned strings captured in the pending trap record and
///        reset it — called once the trap has been delivered or discarded.
void BytecodeVM::clearTrapRecord() {
    for (size_t i = 0; i < trapRecord_.valueSlots.size() && i < trapRecord_.valueOwned.size();
         ++i) {
        if (trapRecord_.valueOwned[i] == 0)
            continue;
        if (trapRecord_.valueSlots[i].ptr &&
            validateStringHandle(trapRecord_.valueSlots[i].ptr, "BytecodeVM::clearTrapRecord")) {
            rt_str_release_maybe(static_cast<rt_string>(trapRecord_.valueSlots[i].ptr));
        }
    }
    trapRecord_ = TrapRecord{};
}

/// @brief Return the VM to a clean pre-execution state: release stack/trap
///        strings, clear the call and EH stacks, rewind sp/fp/alloca, and
///        zero the string-ownership bitmap. (Globals are kept — see
///        @ref releaseOwnedGlobals.)
void BytecodeVM::resetExecutionState() {
    releaseOwnedValueStack();
    clearTrapRecord();
    callStack_.clear();
    ehStack_.clear();
    sp_ = valueStack_.data();
    fp_ = nullptr;
    allocaTop_ = 0;
    pendingTrapErrorCode_ = false;
    valueStackStringOwned_.clearAll();
}

/// @brief Current operand-stack depth of @p frame given stack pointer @p sp
///        (slots pushed since the frame's stack base).
/// @param frame Frame that defines the operand-stack base.
/// @param sp Current stack pointer.
/// @return Number of live operand slots in @p frame.
uint32_t BytecodeVM::operandDepth(const BCFrame &frame, const BCSlot *sp) const {
    return static_cast<uint32_t>(sp - frame.stackBase);
}

/// @brief Verifier guard: trap (InvalidOpcode) unless @p pc is within @p func's
///        code. Part of the untrusted-bytecode safety net.
/// @param func Function whose code bounds apply.
/// @param pc Candidate program counter.
/// @param site Operation name included in a trap.
/// @return `true` when @p pc identifies an instruction word.
bool BytecodeVM::ensurePcInRange(const BytecodeFunction &func, uint32_t pc, const char *site) {
    if (pc < func.code.size())
        return true;
    trap(TrapKind::InvalidOpcode, (std::string(site) + ": program counter out of range").c_str());
    return false;
}

/// @brief Verifier guard: trap unless @p words instruction words starting at
///        @p pc lie within @p func's code (overflow-safe in 64-bit).
/// @param func Function whose code bounds apply.
/// @param pc First word to inspect.
/// @param words Required contiguous word count.
/// @param site Operation name included in a trap.
/// @return `true` when the entire word range is available.
bool BytecodeVM::ensureWordsAvailable(const BytecodeFunction &func,
                                      uint32_t pc,
                                      uint32_t words,
                                      const char *site) {
    const uint64_t end = static_cast<uint64_t>(pc) + static_cast<uint64_t>(words);
    if (end <= func.code.size())
        return true;
    trap(TrapKind::InvalidOpcode, (std::string(site) + ": instruction data out of range").c_str());
    return false;
}

/// @brief Verifier guard: trap unless branch @p target is a valid code offset
///        in @p func.
/// @param func Function whose code bounds apply.
/// @param target Absolute target program counter.
/// @param site Operation name included in a trap.
/// @return `true` when @p target is within the code stream.
bool BytecodeVM::ensureBranchTarget(const BytecodeFunction &func,
                                    uint32_t target,
                                    const char *site) {
    if (target < func.code.size())
        return true;
    trap(TrapKind::InvalidOpcode, (std::string(site) + ": branch target out of range").c_str());
    return false;
}

/// @brief Verifier guard: decode @p instr's stack effect and trap if the
///        operand stack would underflow (too few inputs) or overflow the
///        frame's reserved depth. Run before executing each instruction in
///        untrusted mode so malformed bytecode cannot corrupt the stack.
/// @param frame Current call frame and its declared stack limit.
/// @param sp Current operand stack pointer.
/// @param instr Encoded instruction to validate.
/// @param site Dispatch implementation name included in a trap.
/// @return `true` when the instruction's stack preconditions hold.
bool BytecodeVM::ensureStackForInstruction(const BCFrame &frame,
                                           const BCSlot *sp,
                                           uint32_t instr,
                                           const char *site) {
    uint32_t required = 0;
    int32_t delta = 0;
    const BCOpcode op = decodeOpcode(instr);
    switch (op) {
        case BCOpcode::NOP:
        case BCOpcode::INC_LOCAL:
        case BCOpcode::DEC_LOCAL:
        case BCOpcode::EH_PUSH:
        case BCOpcode::EH_POP:
        case BCOpcode::EH_ENTRY:
        case BCOpcode::TRAP:
            break;
        case BCOpcode::DUP:
        case BCOpcode::POP:
        case BCOpcode::NEG_I64:
        case BCOpcode::NEG_F64:
        case BCOpcode::NOT_I64:
        case BCOpcode::BOOL_TO_I64:
        case BCOpcode::I64_TO_F64:
        case BCOpcode::U64_TO_F64:
        case BCOpcode::F64_TO_I64:
        case BCOpcode::F64_TO_I64_CHK:
        case BCOpcode::F64_TO_U64_CHK:
        case BCOpcode::I64_TO_BOOL:
        case BCOpcode::STR_RETAIN:
        case BCOpcode::STR_RELEASE:
            required = 1;
            delta = (op == BCOpcode::DUP) ? 1 : 0;
            break;
        case BCOpcode::DUP2:
        case BCOpcode::POP2:
            required = 2;
            delta = (op == BCOpcode::DUP2) ? 2 : -2;
            break;
        case BCOpcode::SWAP:
            required = 2;
            break;
        case BCOpcode::ROT3:
            required = 3;
            break;
        case BCOpcode::ADD_I64:
        case BCOpcode::SUB_I64:
        case BCOpcode::MUL_I64:
        case BCOpcode::SDIV_I64:
        case BCOpcode::UDIV_I64:
        case BCOpcode::SREM_I64:
        case BCOpcode::UREM_I64:
        case BCOpcode::ADD_I64_OVF:
        case BCOpcode::SUB_I64_OVF:
        case BCOpcode::MUL_I64_OVF:
        case BCOpcode::SDIV_I64_CHK:
        case BCOpcode::UDIV_I64_CHK:
        case BCOpcode::SREM_I64_CHK:
        case BCOpcode::UREM_I64_CHK:
        case BCOpcode::ADD_F64:
        case BCOpcode::SUB_F64:
        case BCOpcode::MUL_F64:
        case BCOpcode::DIV_F64:
        case BCOpcode::AND_I64:
        case BCOpcode::OR_I64:
        case BCOpcode::XOR_I64:
        case BCOpcode::SHL_I64:
        case BCOpcode::LSHR_I64:
        case BCOpcode::ASHR_I64:
        case BCOpcode::CMP_EQ_I64:
        case BCOpcode::CMP_NE_I64:
        case BCOpcode::CMP_SLT_I64:
        case BCOpcode::CMP_SLE_I64:
        case BCOpcode::CMP_SGT_I64:
        case BCOpcode::CMP_SGE_I64:
        case BCOpcode::CMP_ULT_I64:
        case BCOpcode::CMP_ULE_I64:
        case BCOpcode::CMP_UGT_I64:
        case BCOpcode::CMP_UGE_I64:
        case BCOpcode::CMP_EQ_F64:
        case BCOpcode::CMP_NE_F64:
        case BCOpcode::CMP_LT_F64:
        case BCOpcode::CMP_LE_F64:
        case BCOpcode::CMP_GT_F64:
        case BCOpcode::CMP_GE_F64:
        case BCOpcode::CMP_ORD_F64:
        case BCOpcode::CMP_UNO_F64:
        case BCOpcode::I64_NARROW_CHK:
        case BCOpcode::U64_NARROW_CHK:
        case BCOpcode::STORE_LOCAL:
        case BCOpcode::STORE_LOCAL_W:
        case BCOpcode::STORE_GLOBAL:
        case BCOpcode::JUMP_IF_TRUE:
        case BCOpcode::JUMP_IF_FALSE:
        case BCOpcode::ALLOCA:
        case BCOpcode::ERR_GET_KIND:
        case BCOpcode::ERR_GET_CODE:
        case BCOpcode::ERR_GET_IP:
        case BCOpcode::ERR_GET_LINE:
        case BCOpcode::ERR_GET_MSG:
        case BCOpcode::TRAP_FROM_ERR:
        case BCOpcode::RESUME_SAME:
        case BCOpcode::RESUME_NEXT:
        case BCOpcode::RESUME_LABEL:
            required = 1;
            delta = (op == BCOpcode::STORE_LOCAL || op == BCOpcode::STORE_LOCAL_W ||
                     op == BCOpcode::STORE_GLOBAL || op == BCOpcode::JUMP_IF_TRUE ||
                     op == BCOpcode::JUMP_IF_FALSE || op == BCOpcode::TRAP_FROM_ERR ||
                     op == BCOpcode::RESUME_SAME || op == BCOpcode::RESUME_NEXT ||
                     op == BCOpcode::RESUME_LABEL)
                        ? -1
                        : 0;
            break;
        case BCOpcode::GEP:
        case BCOpcode::STORE_I8_MEM:
        case BCOpcode::STORE_I16_MEM:
        case BCOpcode::STORE_I32_MEM:
        case BCOpcode::STORE_I64_MEM:
        case BCOpcode::STORE_F64_MEM:
        case BCOpcode::STORE_PTR_MEM:
        case BCOpcode::STORE_STR_MEM:
            required = 2;
            delta = -1;
            break;
        case BCOpcode::ARR_I32_GET_FAST:
        case BCOpcode::ARR_I64_GET_FAST:
        case BCOpcode::ARR_F64_GET_FAST:
            required = 2;
            delta = -1;
            break;
        case BCOpcode::IDX_CHK:
        case BCOpcode::SELECT:
            required = 3;
            delta = -2;
            break;
        case BCOpcode::ARR_I32_SET_FAST:
        case BCOpcode::ARR_I64_SET_FAST:
        case BCOpcode::ARR_F64_SET_FAST:
            required = 3;
            delta = -3;
            break;
        case BCOpcode::LOAD_LOCAL:
        case BCOpcode::LOAD_LOCAL_W:
        case BCOpcode::LOAD_I8:
        case BCOpcode::LOAD_I16:
        case BCOpcode::LOAD_I32:
        case BCOpcode::LOAD_I64:
        case BCOpcode::LOAD_F64:
        case BCOpcode::LOAD_STR:
        case BCOpcode::LOAD_NULL:
        case BCOpcode::LOAD_ZERO:
        case BCOpcode::LOAD_ONE:
        case BCOpcode::LOAD_GLOBAL:
        case BCOpcode::LOAD_GLOBAL_ADDR:
        case BCOpcode::TRAP_KIND:
            delta = 1;
            break;
        case BCOpcode::LOAD_I8_MEM:
        case BCOpcode::LOAD_I16_MEM:
        case BCOpcode::LOAD_I32_MEM:
        case BCOpcode::LOAD_I64_MEM:
        case BCOpcode::LOAD_F64_MEM:
        case BCOpcode::LOAD_PTR_MEM:
        case BCOpcode::LOAD_STR_MEM:
        case BCOpcode::RETURN:
        case BCOpcode::SWITCH:
            required = 1;
            delta = (op == BCOpcode::RETURN || op == BCOpcode::SWITCH) ? -1 : 0;
            break;
        case BCOpcode::CALL: {
            const uint16_t funcIdx = decodeArg16(instr);
            if (funcIdx >= module_->functions.size()) {
                trap(TrapKind::RuntimeError, "Invalid function index");
                return false;
            }
            const BytecodeFunction &callee = module_->functions[funcIdx];
            required = callee.numParams;
            delta = callee.hasReturn ? (1 - static_cast<int32_t>(required))
                                     : -static_cast<int32_t>(required);
            break;
        }
        case BCOpcode::CALL_NATIVE: {
            const uint8_t encodedArgCount = decodeArg8_0(instr);
            const uint16_t nativeIdx = decodeArg16_1(instr);
            if (nativeIdx >= module_->nativeFuncs.size()) {
                trap(TrapKind::RuntimeError, "Invalid native function index");
                return false;
            }
            const NativeFuncRef &ref = module_->nativeFuncs[nativeIdx];
            if (encodedArgCount != ref.paramCount) {
                trap(TrapKind::RuntimeError, "CALL_NATIVE encoded arity mismatch");
                return false;
            }
            required = ref.paramCount;
            delta = ref.hasReturn ? (1 - static_cast<int32_t>(required))
                                  : -static_cast<int32_t>(required);
            break;
        }
        case BCOpcode::CALL_INDIRECT:
            required = static_cast<uint32_t>(decodeArg8_0(instr)) + 1;
            delta = 0;
            break;
        case BCOpcode::RETURN_VOID:
            delta = 0;
            break;
        case BCOpcode::JUMP:
        case BCOpcode::JUMP_LONG:
            break;
        default:
            break;
    }

    const uint32_t depth = operandDepth(frame, sp);
    if (depth < required) {
        trap(TrapKind::StackOverflow,
             (std::string(site) + ": operand stack underflow at " + opcodeName(op) + " in " +
              frame.func->name + " @pc=" + std::to_string(frame.pc) +
              " depth=" + std::to_string(depth) + " required=" + std::to_string(required) +
              " max=" + std::to_string(frame.func->maxStack))
                 .c_str());
        return false;
    }

    if (delta > 0 && depth + static_cast<uint32_t>(delta) > frame.func->maxStack) {
        trap(TrapKind::StackOverflow,
             (std::string(site) + ": operand stack overflow at " + opcodeName(op) + " in " +
              frame.func->name + " @pc=" + std::to_string(frame.pc) +
              " depth=" + std::to_string(depth) + " delta=" + std::to_string(delta) +
              " max=" + std::to_string(frame.func->maxStack))
                 .c_str());
        return false;
    }

    return true;
}

/// @brief Verifier guard: trap unless the operand-stack depth at a call site
///        exactly equals the callee's declared parameter count.
/// @param func Candidate callee.
/// @param caller Current caller frame, or null for an entry call.
/// @param sp Stack pointer after arguments have been pushed.
/// @param site Call path included in a trap.
/// @return `true` when the call has exactly the declared number of arguments.
bool BytecodeVM::ensureCallArity(const BytecodeFunction *func,
                                 const BCFrame *caller,
                                 const BCSlot *sp,
                                 const char *site) {
    if (!func) {
        trap(TrapKind::RuntimeError, (std::string(site) + ": null callee").c_str());
        return false;
    }

    const uint32_t depth =
        caller ? operandDepth(*caller, sp) : static_cast<uint32_t>(sp - valueStack_.data());
    if (depth != func->numParams) {
        trap(TrapKind::RuntimeError, (std::string(site) + ": call arity mismatch").c_str());
        return false;
    }

    return true;
}

/// @brief Verifier guard: validate the callee's frame metadata (locals ≥
///        params) and that pushing its frame won't exceed stack limits,
///        before a new BCFrame is created.
/// @param func Candidate callee.
/// @param sp Stack pointer after arguments have been pushed.
/// @param site Call path included in a trap.
/// @return `true` when the frame fits in the fixed value-stack allocation.
bool BytecodeVM::ensureFrameFootprint(const BytecodeFunction *func,
                                      const BCSlot *sp,
                                      const char *site) {
    if (!func) {
        trap(TrapKind::RuntimeError, (std::string(site) + ": null callee").c_str());
        return false;
    }
    if (func->numLocals < func->numParams) {
        trap(TrapKind::InvalidOpcode, (std::string(site) + ": invalid frame metadata").c_str());
        return false;
    }
    BCSlot *localsStart = const_cast<BCSlot *>(sp) - func->numParams;
    BCSlot *stackBase = localsStart + func->numLocals;
    BCSlot *stackLimit = stackBase + func->maxStack;
    if (localsStart < valueStack_.data() || stackLimit > valueStack_.data() + valueStack_.size()) {
        trap(TrapKind::StackOverflow, (std::string(site) + ": frame exceeds value stack").c_str());
        return false;
    }
    return true;
}

/// @brief Execute the named function with @p args; traps if no module is
///        loaded or the name is unknown. Convenience over the pointer overload.
/// @param funcName Exact name in the loaded module's function index.
/// @param args Borrowed argument bits; the callee receives them as non-owning locals.
/// @return The function's return slot (default-constructed on trap).
BCSlot BytecodeVM::exec(const std::string &funcName, const std::vector<BCSlot> &args) {
    if (!module_) {
        if (loadFailed_ && state_ == VMState::Trapped)
            return BCSlot{};
        trap(TrapKind::RuntimeError, "No module loaded");
        return BCSlot{};
    }

    const BytecodeFunction *func = module_->findFunction(funcName);
    if (!func) {
        trap(TrapKind::RuntimeError, "Function not found");
        return BCSlot{};
    }

    return exec(func, args);
}

/// @brief Execute @p func as a fresh top-level invocation.
/// @details Validates module/function/arity, installs the thread-local
///          active-VM and active-module context (so Thread.Start and other
///          runtime handlers can re-enter bytecode), resets execution state,
///          pushes @p args as the entry frame's initial locals, runs to
///          completion, and returns the result slot (default on trap).
/// @param func Function owned by the currently loaded module.
/// @param args Argument slots; count must equal `func->numParams`.
/// @return Return slot on a clean value-returning halt, otherwise a zeroed slot.
BCSlot BytecodeVM::exec(const BytecodeFunction *func, const std::vector<BCSlot> &args) {
    registerUnifiedVmRuntimeHandlers();
    if (!module_) {
        if (loadFailed_ && state_ == VMState::Trapped)
            return BCSlot{};
        trap(TrapKind::RuntimeError, "No module loaded");
        return BCSlot{};
    }
    if (!func) {
        trap(TrapKind::RuntimeError, "Null function entry");
        return BCSlot{};
    }
    if (!functionBelongsToModule(func)) {
        trap(TrapKind::RuntimeError, "Function entry does not belong to loaded module");
        return BCSlot{};
    }
    if (args.size() != func->numParams) {
        trap(TrapKind::RuntimeError, "Function entry arity mismatch");
        return BCSlot{};
    }

    // Set up thread-local context so Thread.Start handler can find us
    ActiveBytecodeVMGuard vmGuard(this);
    ActiveBytecodeModuleGuard moduleGuard(module_);

    // Reset state
    state_ = VMState::Ready;
    trapKind_ = TrapKind::None;
    currentErrorCode_ = 0;
    pendingTrapErrorCode_ = false;
    trapMessage_.clear();
    resetExecutionState();

    // Push arguments onto stack as initial locals
    for (const auto &arg : args) {
        *sp_++ = arg;
        setSlotOwnsString(sp_ - 1, false);
    }

    // Call the function
    call(func);

    // Check if call setup failed (e.g., stack overflow in first call)
    if (state_ == VMState::Trapped || !fp_) {
        if (!fp_ && state_ != VMState::Trapped) {
            trap(TrapKind::RuntimeError, "Frame setup failed");
        }
        return BCSlot{};
    }

    // Run interpreter - use threaded dispatch if available and enabled
#if defined(__GNUC__) || defined(__clang__)
    if (useThreadedDispatch_) {
        runThreaded();
    } else {
        run();
    }
#else
    run();
#endif

    // Return result. Void functions do not leave a meaningful result slot.
    if (state_ == VMState::Halted && !func->hasReturn) {
        return BCSlot{};
    }
    if (state_ == VMState::Halted && sp_ > valueStack_.data()) {
        return *(sp_ - 1);
    }
    return BCSlot{};
}

/// @brief Invoke a void bytecode function without resetting the active VM.
/// @details When called during bytecode execution, suspends the caller frame,
///          executes the callback to a depth boundary, and restores the prior
///          stack pointer and VM state. With no active frame it delegates to
///          the top-level @ref exec path.
/// @param func Void-returning callback in the loaded module.
/// @param args Callback arguments; count must equal `func->numParams`.
/// @return `true` on normal callback completion; `false` after a trap.
bool BytecodeVM::invokeVoidReentrant(const BytecodeFunction *func,
                                     const std::vector<BCSlot> &args) {
    registerUnifiedVmRuntimeHandlers();
    if (!module_) {
        if (!(loadFailed_ && state_ == VMState::Trapped))
            trap(TrapKind::RuntimeError, "No module loaded");
        return false;
    }
    if (!func) {
        trap(TrapKind::RuntimeError, "Null function entry");
        return false;
    }
    if (!functionBelongsToModule(func)) {
        trap(TrapKind::RuntimeError, "Function entry does not belong to loaded module");
        return false;
    }
    if (func->hasReturn) {
        trap(TrapKind::RuntimeError, "Reentrant callback must return void");
        return false;
    }
    if (args.size() != func->numParams) {
        trap(TrapKind::RuntimeError, "Function entry arity mismatch");
        return false;
    }
    if (!fp_ || callStack_.empty())
        return exec(func, args).i64 == 0 && state_ != VMState::Trapped;
    if (state_ == VMState::Trapped)
        return false;

    BCSlot *savedSp = sp_;
    if (savedSp + static_cast<std::ptrdiff_t>(args.size()) >
        valueStack_.data() + valueStack_.size()) {
        trap(TrapKind::StackOverflow, "reentrant callback arguments exceed value stack");
        return false;
    }

    const VMState savedState = state_;
    const size_t savedStopDepth = reentrantStopDepth_;
    BCSlot *savedReturnSp = reentrantReturnSp_;
    const size_t callerDepth = callStack_.size();

    ActiveBytecodeVMGuard vmGuard(this);
    ActiveBytecodeModuleGuard moduleGuard(module_);
    reentrantStopDepth_ = callerDepth;
    reentrantReturnSp_ = savedSp;
    state_ = VMState::Running;

    for (const auto &arg : args) {
        *sp_++ = arg;
        setSlotOwnsString(sp_ - 1, false);
    }

    callReentrant(func);
    if (state_ != VMState::Trapped && fp_) {
#if defined(__GNUC__) || defined(__clang__)
        if (useThreadedDispatch_) {
            runThreaded();
        } else {
            run();
        }
#else
        run();
#endif
    }

    reentrantStopDepth_ = savedStopDepth;
    reentrantReturnSp_ = savedReturnSp;
    if (state_ == VMState::Trapped)
        return false;
    sp_ = savedSp;
    state_ = savedState;
    return true;
}

/// @brief Invoke a value-returning bytecode function without resetting the active VM.
/// @details Preserves the suspended caller's stack and interpreter state while
///          running the callback. The callback's result is copied out before
///          its temporary operand slot is discarded.
/// @param func Value-returning callback in the loaded module.
/// @param args Callback arguments; count must equal `func->numParams`.
/// @param out Optional destination initialized to a zeroed slot before execution.
/// @return `true` on normal completion; `false` after a trap.
bool BytecodeVM::invokeValueReentrant(const BytecodeFunction *func,
                                      const std::vector<BCSlot> &args,
                                      BCSlot *out) {
    registerUnifiedVmRuntimeHandlers();
    if (out)
        *out = BCSlot{};
    if (!module_) {
        if (!(loadFailed_ && state_ == VMState::Trapped))
            trap(TrapKind::RuntimeError, "No module loaded");
        return false;
    }
    if (!func) {
        trap(TrapKind::RuntimeError, "Null function entry");
        return false;
    }
    if (!functionBelongsToModule(func)) {
        trap(TrapKind::RuntimeError, "Function entry does not belong to loaded module");
        return false;
    }
    if (!func->hasReturn) {
        trap(TrapKind::RuntimeError, "Reentrant callback must return a value");
        return false;
    }
    if (args.size() != func->numParams) {
        trap(TrapKind::RuntimeError, "Function entry arity mismatch");
        return false;
    }
    if (!fp_ || callStack_.empty()) {
        BCSlot result = exec(func, args);
        if (state_ == VMState::Trapped)
            return false;
        if (out)
            *out = result;
        return true;
    }
    if (state_ == VMState::Trapped)
        return false;

    BCSlot *savedSp = sp_;
    if (savedSp + static_cast<std::ptrdiff_t>(args.size()) >
        valueStack_.data() + valueStack_.size()) {
        trap(TrapKind::StackOverflow, "reentrant callback arguments exceed value stack");
        return false;
    }

    const VMState savedState = state_;
    const size_t savedStopDepth = reentrantStopDepth_;
    BCSlot *savedReturnSp = reentrantReturnSp_;
    const size_t callerDepth = callStack_.size();

    ActiveBytecodeVMGuard vmGuard(this);
    ActiveBytecodeModuleGuard moduleGuard(module_);
    reentrantStopDepth_ = callerDepth;
    reentrantReturnSp_ = savedSp;
    state_ = VMState::Running;

    for (const auto &arg : args) {
        *sp_++ = arg;
        setSlotOwnsString(sp_ - 1, false);
    }

    callReentrant(func);
    if (state_ != VMState::Trapped && fp_) {
#if defined(__GNUC__) || defined(__clang__)
        if (useThreadedDispatch_) {
            runThreaded();
        } else {
            run();
        }
#else
        run();
#endif
    }

    reentrantStopDepth_ = savedStopDepth;
    reentrantReturnSp_ = savedReturnSp;
    if (state_ == VMState::Trapped)
        return false;
    // returnValueFromFrame restored sp to the boundary and pushed the result there.
    if (out && sp_ > savedSp)
        *out = *savedSp;
    setSlotOwnsString(savedSp, false);
    sp_ = savedSp;
    state_ = savedState;
    return true;
}

/// @brief Portable interpreter loop: the `switch`-based fallback executed when
///        threaded dispatch is unavailable or disabled.
/// @details Fetches/decodes/executes one instruction per iteration until the
///          VM leaves the Running state (Halted on return from the entry
///          frame, or Trapped). @ref runThreaded is the faster path with
///          identical semantics.
void BytecodeVM::run() {
    state_ = VMState::Running;

    while (state_ == VMState::Running) {
        if (!fp_ || !fp_->func)
            return;

        if (!trustedDispatch_ && !ensurePcInRange(*fp_->func, fp_->pc, "BytecodeVM::run(fetch)")) {
            return;
        }

        if (checkBreakpoint()) {
            state_ = VMState::Halted;
            return;
        }

        // Fetch instruction
        uint32_t instr = fp_->func->code[fp_->pc];
        if (!trustedDispatch_ && !ensureStackForInstruction(*fp_, sp_, instr, "BytecodeVM::run"))
            return;
        fp_->pc++;
        BCOpcode op = decodeOpcode(instr);

        ++instrCount_;
        if (maxInstrCount_ != 0 && instrCount_ > maxInstrCount_) {
            trap(TrapKind::Interrupt, "VM: step limit exceeded");
            continue;
        }

        // Trap unknown (non-enumerator) opcode bytes here so the dispatch switch
        // below can omit a `default:` and let -Wswitch enforce that every defined
        // opcode has an explicit handler (compile-time completeness).
        if (!isKnownOpcode(static_cast<uint8_t>(op))) {
            trap(TrapKind::InvalidOpcode, "Unknown opcode");
            continue;
        }

        switch (op) {
            //==================================================================
            // Stack Operations
            //==================================================================
            case BCOpcode::NOP:
                break;

            case BCOpcode::DUP:
                duplicateTopSlot("BytecodeVM::DUP");
                break;

            case BCOpcode::DUP2:
                duplicateTopTwoSlots("BytecodeVM::DUP2");
                break;

            case BCOpcode::POP:
                popOwnedSlots(1);
                break;

            case BCOpcode::POP2:
                popOwnedSlots(2);
                break;

            case BCOpcode::SWAP: {
                swapTopTwoSlots();
                break;
            }

            case BCOpcode::ROT3: {
                rotateTopThreeSlots();
                break;
            }

            //==================================================================
            // Local Variable Operations
            //==================================================================
            case BCOpcode::LOAD_LOCAL: {
                uint8_t idx = decodeArg8_0(instr);
                if (!ensureLocalIndex(idx, "BytecodeVM::LOAD_LOCAL"))
                    break;
                pushLocal(idx, "BytecodeVM::LOAD_LOCAL");
                break;
            }

            case BCOpcode::STORE_LOCAL: {
                uint8_t idx = decodeArg8_0(instr);
                if (!ensureLocalIndex(idx, "BytecodeVM::STORE_LOCAL"))
                    break;
                storeLocal(idx, "BytecodeVM::STORE_LOCAL");
                break;
            }

            case BCOpcode::LOAD_LOCAL_W: {
                uint16_t idx = decodeArg16(instr);
                if (!ensureLocalIndex(idx, "BytecodeVM::LOAD_LOCAL_W"))
                    break;
                pushLocal(idx, "BytecodeVM::LOAD_LOCAL_W");
                break;
            }

            case BCOpcode::STORE_LOCAL_W: {
                uint16_t idx = decodeArg16(instr);
                if (!ensureLocalIndex(idx, "BytecodeVM::STORE_LOCAL_W"))
                    break;
                storeLocal(idx, "BytecodeVM::STORE_LOCAL_W");
                break;
            }

            case BCOpcode::INC_LOCAL: {
                uint8_t idx = decodeArg8_0(instr);
                if (!ensureLocalIndex(idx, "BytecodeVM::INC_LOCAL"))
                    break;
                fp_->locals[idx].i64 = wrappingAdd(fp_->locals[idx].i64, 1);
                break;
            }

            case BCOpcode::DEC_LOCAL: {
                uint8_t idx = decodeArg8_0(instr);
                if (!ensureLocalIndex(idx, "BytecodeVM::DEC_LOCAL"))
                    break;
                fp_->locals[idx].i64 = wrappingSub(fp_->locals[idx].i64, 1);
                break;
            }

            //==================================================================
            // Constant Loading
            //==================================================================
            case BCOpcode::LOAD_I8: {
                int8_t val = decodeArgI8_0(instr);
                sp_->i64 = val;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::LOAD_I16: {
                int16_t val = decodeArgI16(instr);
                sp_->i64 = val;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::LOAD_I32: {
                if (!ensureWordsAvailable(*fp_->func, fp_->pc, 1, "BytecodeVM::LOAD_I32"))
                    return;
                int32_t val = static_cast<int32_t>(fp_->func->code[fp_->pc++]);
                sp_->i64 = val;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::LOAD_I64: {
                uint16_t idx = decodeArg16(instr);
                if (idx >= module_->i64Pool.size()) {
                    trap(TrapKind::InvalidOpcode, "LOAD_I64 constant index out of range");
                    break;
                }
                sp_->i64 = module_->i64Pool[idx];
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::LOAD_F64: {
                uint16_t idx = decodeArg16(instr);
                if (idx >= module_->f64Pool.size()) {
                    trap(TrapKind::InvalidOpcode, "LOAD_F64 constant index out of range");
                    break;
                }
                sp_->f64 = module_->f64Pool[idx];
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::LOAD_NULL:
                sp_->ptr = nullptr;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;

            case BCOpcode::LOAD_ZERO:
                sp_->i64 = 0;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;

            case BCOpcode::LOAD_ONE:
                sp_->i64 = 1;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;

            //==================================================================
            // Integer Arithmetic
            //==================================================================
            case BCOpcode::ADD_I64:
                sp_[-2].i64 = wrappingAdd(sp_[-2].i64, sp_[-1].i64);
                sp_--;
                break;

            case BCOpcode::SUB_I64:
                sp_[-2].i64 = wrappingSub(sp_[-2].i64, sp_[-1].i64);
                sp_--;
                break;

            case BCOpcode::MUL_I64:
                sp_[-2].i64 = wrappingMul(sp_[-2].i64, sp_[-1].i64);
                sp_--;
                break;

            case BCOpcode::SDIV_I64: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeSignedDiv(sp_[-2].i64, sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault,
                             fault == TrapKind::DivideByZero
                                 ? "division by zero"
                                 : "Overflow: integer division overflow");
                    }
                    break;
                }
                sp_[-2].i64 = result;
                sp_--;
            } break;

            case BCOpcode::UDIV_I64: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeUnsignedDiv(sp_[-2].i64, sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault, "division by zero");
                    }
                    break;
                }
                sp_[-2].i64 = result;
                sp_--;
            } break;

            case BCOpcode::SREM_I64: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeSignedRem(sp_[-2].i64, sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault,
                             fault == TrapKind::DivideByZero
                                 ? "division by zero"
                                 : "Overflow: integer remainder overflow");
                    }
                    break;
                }
                sp_[-2].i64 = result;
                sp_--;
            } break;

            case BCOpcode::UREM_I64: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeUnsignedRem(sp_[-2].i64, sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault, "division by zero");
                    }
                    break;
                }
                sp_[-2].i64 = result;
                sp_--;
            } break;

            case BCOpcode::NEG_I64: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeNegate(sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault, "Overflow: integer negation overflow");
                    }
                    break;
                }
                sp_[-1].i64 = result;
            } break;

            case BCOpcode::ADD_I64_OVF: {
                const auto result = il::semantics::checkedAdd(
                    sp_[-2].i64,
                    sp_[-1].i64,
                    detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault, "Overflow: integer overflow in add");
                    }
                    break;
                }
                sp_[-2].i64 = result.value;
                sp_--;
                break;
            }

            case BCOpcode::SUB_I64_OVF: {
                const auto result = il::semantics::checkedSub(
                    sp_[-2].i64,
                    sp_[-1].i64,
                    detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault, "Overflow: integer overflow in sub");
                    }
                    break;
                }
                sp_[-2].i64 = result.value;
                sp_--;
                break;
            }

            case BCOpcode::MUL_I64_OVF: {
                const auto result = il::semantics::checkedMul(
                    sp_[-2].i64,
                    sp_[-1].i64,
                    detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault, "Overflow: integer overflow in mul");
                    }
                    break;
                }
                sp_[-2].i64 = result.value;
                sp_--;
                break;
            }

            case BCOpcode::SDIV_I64_CHK: {
                const auto result =
                    il::semantics::signedDiv(sp_[-2].i64,
                                             sp_[-1].i64,
                                             detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault,
                             fault == TrapKind::DivideByZero ? "division by zero"
                                                             : "Overflow: integer overflow in div");
                    }
                    break;
                }
                sp_[-2].i64 = result.value;
                sp_--;
            } break;

            case BCOpcode::UDIV_I64_CHK: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeUnsignedDiv(sp_[-2].i64, sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault, "division by zero");
                    }
                    break;
                }
                sp_[-2].i64 = result;
                sp_--;
            } break;

            case BCOpcode::SREM_I64_CHK: {
                const auto result =
                    il::semantics::signedRem(sp_[-2].i64,
                                             sp_[-1].i64,
                                             detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault,
                             fault == TrapKind::DivideByZero ? "division by zero"
                                                             : "Overflow: integer overflow in rem");
                    }
                    break;
                }
                sp_[-2].i64 = result.value;
                sp_--;
            } break;

            case BCOpcode::UREM_I64_CHK: {
                int64_t result = 0;
                TrapKind fault = TrapKind::None;
                if (!safeUnsignedRem(sp_[-2].i64, sp_[-1].i64, result, fault)) {
                    if (!dispatchTrap(fault)) {
                        trap(fault, "division by zero");
                    }
                    break;
                }
                sp_[-2].i64 = result;
                sp_--;
            } break;

            case BCOpcode::IDX_CHK: {
                const auto result = il::semantics::boundsCheck(
                    sp_[-3].i64,
                    sp_[-2].i64,
                    sp_[-1].i64,
                    detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault, "index out of bounds");
                    }
                    break;
                }
                sp_[-3].i64 = result.value;
                sp_ -= 2;
                break;
            }

            case BCOpcode::SELECT:
                // Whole-slot copy so integer and floating payloads both pass.
                sp_[-3] = (sp_[-3].i64 != 0) ? sp_[-2] : sp_[-1];
                sp_ -= 2;
                break;

            //==================================================================
            // Float Arithmetic
            //==================================================================
            case BCOpcode::ADD_F64:
                sp_[-2].f64 = sp_[-2].f64 + sp_[-1].f64;
                sp_--;
                break;

            case BCOpcode::SUB_F64:
                sp_[-2].f64 = sp_[-2].f64 - sp_[-1].f64;
                sp_--;
                break;

            case BCOpcode::MUL_F64:
                sp_[-2].f64 = sp_[-2].f64 * sp_[-1].f64;
                sp_--;
                break;

            case BCOpcode::DIV_F64:
                sp_[-2].f64 = sp_[-2].f64 / sp_[-1].f64;
                sp_--;
                break;

            case BCOpcode::NEG_F64:
                sp_[-1].f64 = -sp_[-1].f64;
                break;

            //==================================================================
            // Bitwise Operations
            //==================================================================
            case BCOpcode::AND_I64:
                sp_[-2].i64 = sp_[-2].i64 & sp_[-1].i64;
                sp_--;
                break;

            case BCOpcode::OR_I64:
                sp_[-2].i64 = sp_[-2].i64 | sp_[-1].i64;
                sp_--;
                break;

            case BCOpcode::XOR_I64:
                sp_[-2].i64 = sp_[-2].i64 ^ sp_[-1].i64;
                sp_--;
                break;

            case BCOpcode::NOT_I64:
                sp_[-1].i64 = ~sp_[-1].i64;
                break;

            case BCOpcode::SHL_I64:
                sp_[-2].i64 = wrappingShl(sp_[-2].i64, sp_[-1].i64);
                sp_--;
                break;

            case BCOpcode::LSHR_I64:
                sp_[-2].i64 = il::semantics::logicalShiftRight(sp_[-2].i64, sp_[-1].i64);
                sp_--;
                break;

            case BCOpcode::ASHR_I64:
                sp_[-2].i64 = arithmeticShr(sp_[-2].i64, sp_[-1].i64);
                sp_--;
                break;

            //==================================================================
            // Integer Comparisons
            //==================================================================
            case BCOpcode::CMP_EQ_I64:
                sp_[-2].i64 = (sp_[-2].i64 == sp_[-1].i64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_NE_I64:
                sp_[-2].i64 = (sp_[-2].i64 != sp_[-1].i64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_SLT_I64:
                sp_[-2].i64 = (sp_[-2].i64 < sp_[-1].i64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_SLE_I64:
                sp_[-2].i64 = (sp_[-2].i64 <= sp_[-1].i64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_SGT_I64:
                sp_[-2].i64 = (sp_[-2].i64 > sp_[-1].i64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_SGE_I64:
                sp_[-2].i64 = (sp_[-2].i64 >= sp_[-1].i64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_ULT_I64:
                sp_[-2].i64 =
                    (static_cast<uint64_t>(sp_[-2].i64) < static_cast<uint64_t>(sp_[-1].i64)) ? 1
                                                                                              : 0;
                sp_--;
                break;

            case BCOpcode::CMP_ULE_I64:
                sp_[-2].i64 =
                    (static_cast<uint64_t>(sp_[-2].i64) <= static_cast<uint64_t>(sp_[-1].i64)) ? 1
                                                                                               : 0;
                sp_--;
                break;

            case BCOpcode::CMP_UGT_I64:
                sp_[-2].i64 =
                    (static_cast<uint64_t>(sp_[-2].i64) > static_cast<uint64_t>(sp_[-1].i64)) ? 1
                                                                                              : 0;
                sp_--;
                break;

            case BCOpcode::CMP_UGE_I64:
                sp_[-2].i64 =
                    (static_cast<uint64_t>(sp_[-2].i64) >= static_cast<uint64_t>(sp_[-1].i64)) ? 1
                                                                                               : 0;
                sp_--;
                break;

            //==================================================================
            // Float Comparisons
            //==================================================================
            case BCOpcode::CMP_EQ_F64:
                sp_[-2].i64 = (sp_[-2].f64 == sp_[-1].f64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_NE_F64:
                sp_[-2].i64 = (sp_[-2].f64 != sp_[-1].f64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_LT_F64:
                sp_[-2].i64 = (sp_[-2].f64 < sp_[-1].f64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_LE_F64:
                sp_[-2].i64 = (sp_[-2].f64 <= sp_[-1].f64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_GT_F64:
                sp_[-2].i64 = (sp_[-2].f64 > sp_[-1].f64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_GE_F64:
                sp_[-2].i64 = (sp_[-2].f64 >= sp_[-1].f64) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_ORD_F64:
                // Ordered: true when neither operand is NaN.
                sp_[-2].i64 = (!std::isnan(sp_[-2].f64) && !std::isnan(sp_[-1].f64)) ? 1 : 0;
                sp_--;
                break;

            case BCOpcode::CMP_UNO_F64:
                // Unordered: true when either operand is NaN.
                sp_[-2].i64 = (std::isnan(sp_[-2].f64) || std::isnan(sp_[-1].f64)) ? 1 : 0;
                sp_--;
                break;

            //==================================================================
            // Type Conversions
            //==================================================================
            case BCOpcode::I64_TO_F64:
                sp_[-1].f64 = static_cast<double>(sp_[-1].i64);
                break;

            case BCOpcode::U64_TO_F64:
                sp_[-1].f64 = static_cast<double>(static_cast<uint64_t>(sp_[-1].i64));
                break;

            case BCOpcode::F64_TO_I64: {
                int64_t converted = 0;
                TrapKind fault = TrapKind::None;
                if (!truncF64ToI64(sp_[-1].f64, converted, fault)) {
                    trapOrDispatch(fault,
                                   fault == TrapKind::InvalidCast
                                       ? "InvalidCast: invalid float to int conversion"
                                       : "Overflow: float to int conversion overflow");
                    break;
                }
                sp_[-1].i64 = converted;
            } break;

            case BCOpcode::F64_TO_I64_CHK: {
                const auto result = il::semantics::fpToSiRte(
                    sp_[-1].f64, detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    trapOrDispatch(fault,
                                   fault == TrapKind::InvalidCast
                                       ? "InvalidCast: invalid float to int conversion"
                                       : "Overflow: float to int conversion overflow");
                    break;
                }
                sp_[-1].i64 = result.value;
                break;
            }

            case BCOpcode::F64_TO_U64_CHK: {
                const auto result = il::semantics::fpToUiRte(
                    sp_[-1].f64, detail::decodeArithmeticWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    trapOrDispatch(fault,
                                   fault == TrapKind::InvalidCast
                                       ? "InvalidCast: invalid float to uint conversion"
                                       : "Overflow: float to uint conversion overflow");
                    break;
                }
                sp_[-1].i64 = result.value;
                break;
            }

            case BCOpcode::I64_NARROW_CHK: {
                const auto result = il::semantics::signedNarrow(
                    sp_[-1].i64, detail::decodeNarrowWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault, "InvalidCast: signed narrow conversion out of range");
                    }
                    break;
                }
                sp_[-1].i64 = result.value;
                break;
            }

            case BCOpcode::U64_NARROW_CHK: {
                const auto result = il::semantics::unsignedNarrow(
                    static_cast<uint64_t>(sp_[-1].i64),
                    detail::decodeNarrowWidthArg(decodeArg8_0(instr)));
                if (!result.ok()) {
                    const TrapKind fault = detail::toBytecodeTrap(result.trap);
                    if (!dispatchTrap(fault)) {
                        trap(fault, "InvalidCast: unsigned narrow conversion out of range");
                    }
                    break;
                }
                sp_[-1].i64 = result.value;
                break;
            }

            case BCOpcode::BOOL_TO_I64:
                // Already i64 with 0 or 1
                break;

            case BCOpcode::I64_TO_BOOL:
                sp_[-1].i64 = (sp_[-1].i64 != 0) ? 1 : 0;
                break;

            //==================================================================
            // Control Flow
            //==================================================================
            case BCOpcode::JUMP: {
                int16_t offset = decodeArgI16(instr);
                uint32_t target = 0;
                if (!computeRelativeTarget(*fp_->func, fp_->pc, offset, target, "BytecodeVM::JUMP"))
                    return;
                fp_->pc = target;
                break;
            }

            case BCOpcode::JUMP_IF_TRUE: {
                int16_t offset = decodeArgI16(instr);
                if ((--sp_)->i64 != 0) {
                    uint32_t target = 0;
                    if (!computeRelativeTarget(
                            *fp_->func, fp_->pc, offset, target, "BytecodeVM::JUMP_IF_TRUE"))
                        return;
                    fp_->pc = target;
                }
                break;
            }

            case BCOpcode::JUMP_IF_FALSE: {
                int16_t offset = decodeArgI16(instr);
                if ((--sp_)->i64 == 0) {
                    uint32_t target = 0;
                    if (!computeRelativeTarget(
                            *fp_->func, fp_->pc, offset, target, "BytecodeVM::JUMP_IF_FALSE"))
                        return;
                    fp_->pc = target;
                }
                break;
            }

            case BCOpcode::JUMP_LONG: {
                int32_t offset = decodeArgI24(instr);
                uint32_t target = 0;
                if (!computeRelativeTarget(
                        *fp_->func, fp_->pc, offset, target, "BytecodeVM::JUMP_LONG"))
                    return;
                fp_->pc = target;
                break;
            }

            case BCOpcode::SWITCH: {
                // Format: SWITCH [numCases:u32] [defaultOffset:i32] [caseVal:i32 caseOffset:i32]...
                // Pop scrutinee from stack
                int32_t scrutinee = static_cast<int32_t>((--sp_)->i64);

                // pc currently points to the word after SWITCH opcode (numCases)
                const uint32_t *code = fp_->func->code.data();
                if (!ensureWordsAvailable(*fp_->func, fp_->pc, 2, "BytecodeVM::SWITCH(header)"))
                    return;
                uint32_t numCases = code[fp_->pc++];

                // Position of default offset word
                uint32_t defaultOffsetPos = fp_->pc++;
                const uint64_t caseWords = static_cast<uint64_t>(numCases) * 2u;
                if (caseWords > std::numeric_limits<uint32_t>::max() ||
                    !ensureWordsAvailable(*fp_->func,
                                          fp_->pc,
                                          static_cast<uint32_t>(caseWords),
                                          "BytecodeVM::SWITCH(cases)")) {
                    return;
                }

                // Search for matching case
                bool found = false;
                for (uint32_t i = 0; i < numCases; ++i) {
                    int32_t caseVal = static_cast<int32_t>(code[fp_->pc++]);
                    uint32_t caseOffsetPos = fp_->pc++;

                    if (caseVal == scrutinee) {
                        // Found matching case - jump to its target
                        // Offset is relative to the offset word position
                        int32_t caseOffset = static_cast<int32_t>(code[caseOffsetPos]);
                        uint32_t target = 0;
                        if (!computeRelativeTarget(*fp_->func,
                                                   caseOffsetPos,
                                                   caseOffset,
                                                   target,
                                                   "BytecodeVM::SWITCH(case)"))
                            return;
                        fp_->pc = target;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    // No match - use default offset
                    int32_t defaultOffset = static_cast<int32_t>(code[defaultOffsetPos]);
                    uint32_t target = 0;
                    if (!computeRelativeTarget(*fp_->func,
                                               defaultOffsetPos,
                                               defaultOffset,
                                               target,
                                               "BytecodeVM::SWITCH(default)"))
                        return;
                    fp_->pc = target;
                }
                break;
            }

            case BCOpcode::CALL: {
                uint16_t funcIdx = decodeArg16(instr);
                if (funcIdx < module_->functions.size()) {
                    call(&module_->functions[funcIdx]);
                } else {
                    trap(TrapKind::RuntimeError, "Invalid function index");
                }
                break;
            }

            case BCOpcode::RETURN: {
                if (!returnValueFromFrame())
                    return;
                break;
            }

            case BCOpcode::RETURN_VOID: {
                if (!returnVoidFromFrame())
                    return;
                break;
            }

            case BCOpcode::CALL_NATIVE: {
                // Instruction format: CALL_NATIVE [argCount:8][nativeIdx:16]
                uint8_t argCount = decodeArg8_0(instr);
                uint16_t nativeIdx = decodeArg16_1(instr);

                if (nativeIdx >= module_->nativeFuncs.size()) {
                    trap(TrapKind::RuntimeError, "Invalid native function index");
                    break;
                }

                const NativeFuncRef &ref = module_->nativeFuncs[nativeIdx];
                if (argCount != ref.paramCount) {
                    trap(TrapKind::RuntimeError, "CALL_NATIVE encoded arity mismatch");
                    break;
                }

                // Set up arguments (they're on the stack)
                BCSlot *args = sp_ - argCount;
                BCSlot result{};

                if (runtimeBridgeEnabled_) {
                    if (!invokeRuntimeBridgeNative(ref, args, argCount, result))
                        break;
                } else {
                    // Look up handler in local registry
                    auto it = nativeHandlers_.find(ref.name);
                    if (it == nativeHandlers_.end()) {
                        trap(TrapKind::RuntimeError, "Native function not registered");
                        break;
                    }
                    // Call the handler
                    it->second(args, argCount, &result);
                }

                dismissConsumedStringArgs(ref, args, argCount);
                releaseCallArgs(args, argCount);

                // Pop arguments
                sp_ -= argCount;

                // Push result if function returns a value
                if (ref.hasReturn) {
                    *sp_++ = result;
                    if (ref.returnsString && result.ptr) {
                        setSlotOwnsString(sp_ - 1, true);
                    } else {
                        setSlotOwnsString(sp_ - 1, false);
                    }
                }
                break;
            }

            case BCOpcode::ARR_I32_GET_FAST: {
                BCSlot *arrSlot = sp_ - 2;
                if (!arrSlot->ptr) {
                    trapOrDispatch(TrapKind::NullPointer, "ARR_I32_GET_FAST on null array");
                    break;
                }
                BCSlot *idxSlot = --sp_;
                const size_t idx = static_cast<size_t>(idxSlot->i64);
                setSlotOwnsString(idxSlot, false);
                int32_t *element = nullptr;
                if (!resolveArrayFastElement<int32_t>(arrSlot->ptr,
                                                      idx,
                                                      element,
                                                      "ARR_I32_GET_FAST index address overflow",
                                                      "BytecodeVM::ARR_I32_GET_FAST"))
                    break;
                arrSlot->i64 = static_cast<int64_t>(*element);
                setSlotOwnsString(arrSlot, false);
                break;
            }

            case BCOpcode::ARR_I32_SET_FAST: {
                BCSlot *arrSlot = sp_ - 3;
                if (!arrSlot->ptr) {
                    trapOrDispatch(TrapKind::NullPointer, "ARR_I32_SET_FAST on null array");
                    break;
                }
                BCSlot *valueSlot = --sp_;
                const int32_t value = static_cast<int32_t>(valueSlot->i64);
                setSlotOwnsString(valueSlot, false);
                BCSlot *idxSlot = --sp_;
                const size_t idx = static_cast<size_t>(idxSlot->i64);
                setSlotOwnsString(idxSlot, false);
                --sp_;
                int32_t *element = nullptr;
                if (!resolveArrayFastElement<int32_t>(arrSlot->ptr,
                                                      idx,
                                                      element,
                                                      "ARR_I32_SET_FAST index address overflow",
                                                      "BytecodeVM::ARR_I32_SET_FAST"))
                    break;
                setSlotOwnsString(arrSlot, false);
                *element = value;
                break;
            }

            case BCOpcode::ARR_I64_GET_FAST: {
                BCSlot *arrSlot = sp_ - 2;
                if (!arrSlot->ptr) {
                    trapOrDispatch(TrapKind::NullPointer, "ARR_I64_GET_FAST on null array");
                    break;
                }
                BCSlot *idxSlot = --sp_;
                const size_t idx = static_cast<size_t>(idxSlot->i64);
                setSlotOwnsString(idxSlot, false);
                int64_t *element = nullptr;
                if (!resolveArrayFastElement<int64_t>(arrSlot->ptr,
                                                      idx,
                                                      element,
                                                      "ARR_I64_GET_FAST index address overflow",
                                                      "BytecodeVM::ARR_I64_GET_FAST"))
                    break;
                arrSlot->i64 = *element;
                setSlotOwnsString(arrSlot, false);
                break;
            }

            case BCOpcode::ARR_I64_SET_FAST: {
                BCSlot *arrSlot = sp_ - 3;
                if (!arrSlot->ptr) {
                    trapOrDispatch(TrapKind::NullPointer, "ARR_I64_SET_FAST on null array");
                    break;
                }
                BCSlot *valueSlot = --sp_;
                const int64_t value = valueSlot->i64;
                setSlotOwnsString(valueSlot, false);
                BCSlot *idxSlot = --sp_;
                const size_t idx = static_cast<size_t>(idxSlot->i64);
                setSlotOwnsString(idxSlot, false);
                --sp_;
                int64_t *element = nullptr;
                if (!resolveArrayFastElement<int64_t>(arrSlot->ptr,
                                                      idx,
                                                      element,
                                                      "ARR_I64_SET_FAST index address overflow",
                                                      "BytecodeVM::ARR_I64_SET_FAST"))
                    break;
                setSlotOwnsString(arrSlot, false);
                *element = value;
                break;
            }

            case BCOpcode::ARR_F64_GET_FAST: {
                BCSlot *arrSlot = sp_ - 2;
                if (!arrSlot->ptr) {
                    trapOrDispatch(TrapKind::NullPointer, "ARR_F64_GET_FAST on null array");
                    break;
                }
                BCSlot *idxSlot = --sp_;
                const size_t idx = static_cast<size_t>(idxSlot->i64);
                setSlotOwnsString(idxSlot, false);
                double *element = nullptr;
                if (!resolveArrayFastElement<double>(arrSlot->ptr,
                                                     idx,
                                                     element,
                                                     "ARR_F64_GET_FAST index address overflow",
                                                     "BytecodeVM::ARR_F64_GET_FAST"))
                    break;
                arrSlot->f64 = *element;
                setSlotOwnsString(arrSlot, false);
                break;
            }

            case BCOpcode::ARR_F64_SET_FAST: {
                BCSlot *arrSlot = sp_ - 3;
                if (!arrSlot->ptr) {
                    trapOrDispatch(TrapKind::NullPointer, "ARR_F64_SET_FAST on null array");
                    break;
                }
                BCSlot *valueSlot = --sp_;
                const double value = valueSlot->f64;
                setSlotOwnsString(valueSlot, false);
                BCSlot *idxSlot = --sp_;
                const size_t idx = static_cast<size_t>(idxSlot->i64);
                setSlotOwnsString(idxSlot, false);
                --sp_;
                double *element = nullptr;
                if (!resolveArrayFastElement<double>(arrSlot->ptr,
                                                     idx,
                                                     element,
                                                     "ARR_F64_SET_FAST index address overflow",
                                                     "BytecodeVM::ARR_F64_SET_FAST"))
                    break;
                setSlotOwnsString(arrSlot, false);
                *element = value;
                break;
            }

            case BCOpcode::CALL_INDIRECT: {
                // Indirect call through function pointer
                // Stack layout: [callee][arg0][arg1]...[argN] <- sp
                uint8_t argCount = decodeArg8_0(instr);

                // Get callee from below the arguments
                BCSlot *callee = sp_ - argCount - 1;
                BCSlot *args = sp_ - argCount;

                // Check if callee is a tagged function pointer (high bit set)
                constexpr uint64_t kFuncPtrTag = 0x8000000000000000ULL;
                uint64_t calleeVal = static_cast<uint64_t>(callee->i64);

                if (calleeVal & kFuncPtrTag) {
                    // Tagged function index - extract and call
                    uint32_t funcIdx = static_cast<uint32_t>(calleeVal & 0x7FFFFFFFULL);
                    if (funcIdx >= module_->functions.size()) {
                        trap(TrapKind::RuntimeError, "Invalid indirect function index");
                        break;
                    }
                    const BytecodeFunction &targetFunc = module_->functions[funcIdx];
                    if (argCount != targetFunc.numParams) {
                        trap(TrapKind::RuntimeError, "Indirect call arity mismatch");
                        break;
                    }

                    // Shift arguments down to overwrite the callee slot
                    for (int i = 0; i < argCount; ++i) {
                        callee[i] = args[i];
                        setSlotOwnsString(callee + i, slotOwnsString(args + i));
                        if (callee + i != args + i)
                            setSlotOwnsString(args + i, false);
                    }
                    sp_ = callee + argCount; // Adjust stack pointer

                    call(&targetFunc);
                } else if (calleeVal == 0) {
                    // Null function pointer
                    trap(TrapKind::NullPointer, "Null indirect callee");
                    break;
                } else {
                    // Unknown pointer format
                    trap(TrapKind::RuntimeError, "Invalid indirect call target");
                    break;
                }
                break;
            }

            //==================================================================
            // Memory Operations (basic support)
            //==================================================================
            case BCOpcode::ALLOCA: {
                // Allocate from the separate alloca buffer (not operand stack)
                // This ensures alloca'd memory survives across function calls
                const int64_t size = (--sp_)->i64;
                void *ptr = nullptr;
                if (!allocateAlloca(size, ptr, "BytecodeVM::ALLOCA"))
                    break;
                sp_->ptr = ptr;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::GEP: {
                int64_t offset = (--sp_)->i64;
                void *adjusted = nullptr;
                if (!addPointerOffset(sp_[-1].ptr, offset, adjusted, "BytecodeVM::GEP"))
                    break;
                sp_[-1].ptr = adjusted;
                break;
            }

            case BCOpcode::LOAD_I64_MEM: {
                void *ptr = sp_[-1].ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int64_t), "BytecodeVM::LOAD_I64_MEM"))
                    break;
                int64_t val;
                std::memcpy(&val, ptr, sizeof(val));
                sp_[-1].i64 = val;
                break;
            }

            case BCOpcode::STORE_I64_MEM: {
                int64_t val = (--sp_)->i64;
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int64_t), "BytecodeVM::STORE_I64_MEM"))
                    break;
                clearGlobalStringOwnershipForRawStore(ptr, sizeof(int64_t));
                std::memcpy(ptr, &val, sizeof(val));
                break;
            }

            case BCOpcode::LOAD_I8_MEM: {
                void *ptr = sp_[-1].ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int8_t), "BytecodeVM::LOAD_I8_MEM"))
                    break;
                int8_t val;
                std::memcpy(&val, ptr, sizeof(val));
                sp_[-1].i64 = val; // Sign extend
                break;
            }

            case BCOpcode::LOAD_I16_MEM: {
                void *ptr = sp_[-1].ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int16_t), "BytecodeVM::LOAD_I16_MEM"))
                    break;
                int16_t val;
                std::memcpy(&val, ptr, sizeof(val));
                sp_[-1].i64 = val; // Sign extend
                break;
            }

            case BCOpcode::LOAD_I32_MEM: {
                void *ptr = sp_[-1].ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int32_t), "BytecodeVM::LOAD_I32_MEM"))
                    break;
                int32_t val;
                std::memcpy(&val, ptr, sizeof(val));
                sp_[-1].i64 = val; // Sign extend
                break;
            }

            case BCOpcode::LOAD_F64_MEM: {
                void *ptr = sp_[-1].ptr;
                if (!ensureMemoryAccess(ptr, sizeof(double), "BytecodeVM::LOAD_F64_MEM"))
                    break;
                double val;
                std::memcpy(&val, ptr, sizeof(val));
                sp_[-1].f64 = val;
                break;
            }

            case BCOpcode::LOAD_PTR_MEM: {
                void *val;
                if (!ensureMemoryAccess(sp_[-1].ptr, sizeof(void *), "BytecodeVM::LOAD_PTR_MEM"))
                    break;
                std::memcpy(&val, sp_[-1].ptr, sizeof(val));
                sp_[-1].ptr = val;
                setSlotOwnsString(sp_ - 1, false);
                break;
            }

            case BCOpcode::LOAD_STR_MEM: {
                rt_string val = nullptr;
                if (!ensureMemoryAccess(sp_[-1].ptr, sizeof(rt_string), "BytecodeVM::LOAD_STR_MEM"))
                    break;
                std::memcpy(&val, sp_[-1].ptr, sizeof(val));
                sp_[-1].ptr = val;
                if (val) {
                    if (!validateStringHandle(val, "BytecodeVM::LOAD_STR_MEM"))
                        break;
                    rt_str_retain_maybe(val);
                    setSlotOwnsString(sp_ - 1, true);
                } else {
                    setSlotOwnsString(sp_ - 1, false);
                }
                break;
            }

            case BCOpcode::STORE_I8_MEM: {
                int8_t val = static_cast<int8_t>((--sp_)->i64);
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int8_t), "BytecodeVM::STORE_I8_MEM"))
                    break;
                clearGlobalStringOwnershipForRawStore(ptr, sizeof(int8_t));
                std::memcpy(ptr, &val, sizeof(val));
                break;
            }

            case BCOpcode::STORE_I16_MEM: {
                int16_t val = static_cast<int16_t>((--sp_)->i64);
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int16_t), "BytecodeVM::STORE_I16_MEM"))
                    break;
                clearGlobalStringOwnershipForRawStore(ptr, sizeof(int16_t));
                std::memcpy(ptr, &val, sizeof(val));
                break;
            }

            case BCOpcode::STORE_I32_MEM: {
                int32_t val = static_cast<int32_t>((--sp_)->i64);
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(int32_t), "BytecodeVM::STORE_I32_MEM"))
                    break;
                clearGlobalStringOwnershipForRawStore(ptr, sizeof(int32_t));
                std::memcpy(ptr, &val, sizeof(val));
                break;
            }

            case BCOpcode::STORE_F64_MEM: {
                double val = (--sp_)->f64;
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(double), "BytecodeVM::STORE_F64_MEM"))
                    break;
                clearGlobalStringOwnershipForRawStore(ptr, sizeof(double));
                std::memcpy(ptr, &val, sizeof(val));
                break;
            }

            case BCOpcode::STORE_PTR_MEM: {
                void *val = (--sp_)->ptr;
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(void *), "BytecodeVM::STORE_PTR_MEM"))
                    break;
                clearGlobalStringOwnershipForRawStore(ptr, sizeof(void *));
                std::memcpy(ptr, &val, sizeof(val));
                setSlotOwnsString(sp_, false);
                setSlotOwnsString(sp_ + 1, false);
                break;
            }

            case BCOpcode::STORE_STR_MEM: {
                BCSlot *valueSlot = --sp_;
                rt_string incoming = static_cast<rt_string>(valueSlot->ptr);
                const bool incomingOwns = slotOwnsString(valueSlot);
                void *ptr = (--sp_)->ptr;
                if (!ensureMemoryAccess(ptr, sizeof(rt_string), "BytecodeVM::STORE_STR_MEM"))
                    break;
                rt_string current = nullptr;
                std::memcpy(&current, ptr, sizeof(current));
                if (current && !validateStringHandle(current, "BytecodeVM::STORE_STR_MEM(current)"))
                    break;
                if (incoming && !validateStringHandle(incoming, "BytecodeVM::STORE_STR_MEM"))
                    break;
                if (incoming && (!incomingOwns || incoming == current))
                    rt_str_retain_maybe(incoming);
                rt_str_release_maybe(current);
                std::memcpy(ptr, &incoming, sizeof(incoming));
                setGlobalStringOwnershipForAddress(ptr, incoming != nullptr);
                setSlotOwnsString(valueSlot, false);
                setSlotOwnsString(sp_, false);
                break;
            }

            //==================================================================
            // Global Variables
            //==================================================================
            case BCOpcode::LOAD_GLOBAL: {
                uint16_t idx = decodeArg16(instr);
                if (!loadGlobal(idx, "BytecodeVM::LOAD_GLOBAL"))
                    break;
                break;
            }

            case BCOpcode::STORE_GLOBAL: {
                uint16_t idx = decodeArg16(instr);
                if (!storeGlobal(idx, "BytecodeVM::STORE_GLOBAL"))
                    break;
                break;
            }

            case BCOpcode::LOAD_GLOBAL_ADDR: {
                uint16_t idx = decodeArg16(instr);
                if (idx >= globals_.size()) {
                    trap(TrapKind::InvalidOpcode, "LOAD_GLOBAL_ADDR index out of range");
                    break;
                }
                sp_->ptr = &globals_[idx];
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            //==================================================================
            // String Operations
            //==================================================================
            case BCOpcode::LOAD_STR: {
                uint16_t idx = decodeArg16(instr);
                if (!module_ || idx >= module_->stringPool.size()) {
                    trap(TrapKind::InvalidOpcode, "LOAD_STR constant index out of range");
                    break;
                }
                sp_->ptr = getStringLiteral(idx);
                if (sp_->ptr) {
                    if (!validateStringHandle(sp_->ptr, "BytecodeVM::LOAD_STR"))
                        break;
                    rt_str_retain_maybe(static_cast<rt_string>(sp_->ptr));
                    setSlotOwnsString(sp_, true);
                } else {
                    setSlotOwnsString(sp_, false);
                }
                sp_++;
                break;
            }

            case BCOpcode::STR_RETAIN:
                if (!retainStringSlot(sp_ - 1, "BytecodeVM::STR_RETAIN"))
                    break;
                break;

            case BCOpcode::STR_RELEASE:
                releaseOwnedString(sp_ - 1);
                sp_--;
                break;

            //==================================================================
            // Exception Handling
            //==================================================================
            case BCOpcode::EH_PUSH: {
                // Handler offset is in the next code word (raw i32 offset)
                const uint32_t *code = fp_->func->code.data();
                if (!ensureWordsAvailable(*fp_->func, fp_->pc, 1, "BytecodeVM::EH_PUSH"))
                    return;
                const uint32_t offsetPc = fp_->pc;
                int32_t offset = static_cast<int32_t>(code[fp_->pc++]);
                uint32_t handlerPc = 0;
                if (!computeRelativeTarget(
                        *fp_->func, offsetPc, offset, handlerPc, "BytecodeVM::EH_PUSH"))
                    return;
                pushExceptionHandler(handlerPc);
                break;
            }

            case BCOpcode::EH_POP:
                popExceptionHandler();
                break;

            case BCOpcode::EH_ENTRY:
                // Handler entry marker - no-op, execution continues
                break;

            case BCOpcode::TRAP: {
                uint8_t kind = decodeArg8_0(instr);
                TrapKind decodedTrapKind = static_cast<TrapKind>(kind);
                if (!dispatchTrap(decodedTrapKind)) {
                    trap(decodedTrapKind, "Unhandled trap");
                }
                break;
            }

            case BCOpcode::TRAP_FROM_ERR: {
                // Pop legacy error code from stack, map it to a structured
                // trap kind, and preserve the original code for diagnostics.
                int64_t code = (--sp_)->i64;
                const il::vm::TrapKind vmTrapKind =
                    il::vm::map_err_to_trap(static_cast<int32_t>(code));
                TrapKind decodedTrapKind = static_cast<TrapKind>(static_cast<int32_t>(vmTrapKind));
                if (!dispatchTrap(decodedTrapKind, static_cast<int32_t>(code))) {
                    trap(decodedTrapKind, "Unhandled trap from error");
                }
                break;
            }

            case BCOpcode::ERR_GET_KIND: {
                // Replace the error token with its trap discriminator.
                // The IL form always provides one Error operand, so this is a
                // consume-one / produce-one transform rather than an extra push.
                setSlotOwnsString(sp_ - 1, false);
                break;
            }

            case BCOpcode::ERR_GET_CODE:
                // Replace the error token with the extracted code.
                sp_[-1].i64 = static_cast<int64_t>(currentErrorCode_);
                break;

            case BCOpcode::ERR_GET_IP:
                sp_[-1].i64 = trapRecord_.valid ? static_cast<int64_t>(trapRecord_.faultPc)
                                                : static_cast<int64_t>(fp_ ? fp_->pc : 0);
                break;

            case BCOpcode::ERR_GET_LINE:
                sp_[-1].i64 = trapRecord_.valid ? static_cast<int64_t>(trapRecord_.faultLine) : -1;
                break;

            case BCOpcode::ERR_GET_MSG: {
                // Replace the error token with an owned string holding the trap message,
                // mirroring the tree-walking VM's ErrGetMsg (vm_current_trap_message()).
                const std::string &msg = trapMessage_;
                sp_[-1].ptr = rt_string_from_bytes(msg.data(), msg.size());
                setSlotOwnsString(sp_ - 1, true);
                break;
            }

            case BCOpcode::RESUME_SAME:
                if (!resumeTrap(false))
                    trap(TrapKind::InvalidOperation, "resume.same: invalid resume token");
                break;

            case BCOpcode::RESUME_NEXT:
                if (!resumeTrap(true))
                    trap(TrapKind::InvalidOperation, "resume.next: invalid resume token");
                break;

            case BCOpcode::RESUME_LABEL: {
                // Resume at a specific label in the current frame. The IL
                // form supplies an explicit resume token operand, so the
                // bytecode op must consume and validate that token before
                // continuing to the destination label.
                BCSlot token = *--sp_;
                setSlotOwnsString(sp_, false);
                if (token.ptr != &trapRecord_ || !trapRecord_.valid) {
                    trap(TrapKind::InvalidOperation, "resume.label: invalid resume token");
                    break;
                }
                clearTrapRecord();
                const uint32_t *code = fp_->func->code.data();
                if (!ensureWordsAvailable(*fp_->func, fp_->pc, 1, "BytecodeVM::RESUME_LABEL"))
                    return;
                const uint32_t offsetPc = fp_->pc;
                int32_t offset = static_cast<int32_t>(code[fp_->pc++]);
                uint32_t target = 0;
                if (!computeRelativeTarget(
                        *fp_->func, offsetPc, offset, target, "BytecodeVM::RESUME_LABEL"))
                    return;
                fp_->pc = target;
                break;
            }

            case BCOpcode::TRAP_KIND: {
                // Push the current trap kind as an I64 for typed-catch comparison.
                // Values 0-11 are aligned with il::vm::TrapKind (vm/Trap.hpp).
                // BC-specific kinds (100+) map to RuntimeError(9) as catch-all.
                uint8_t raw = static_cast<uint8_t>(trapKind_);
                int64_t ilKind = (raw <= 11) ? static_cast<int64_t>(raw) : 9;
                sp_->i64 = ilKind;
                setSlotOwnsString(sp_, false);
                sp_++;
                break;
            }

            case BCOpcode::LINE:
            case BCOpcode::WATCH_VAR:
                break;

            case BCOpcode::BREAKPOINT: {
                const uint32_t breakpointPc = currentFaultPc();
                if (requestDebugPause(true, breakpointPc)) {
                    state_ = VMState::Halted;
                    return;
                }
                break;
            }

            //==================================================================
            // Opcodes with no run()-switch handler — trap. (Same set the
            // threaded VM routes to L_DEFAULT via BC_OPCODE_TRAP.) There is no
            // `default:`: with the unknown-byte guard above, the switch covers
            // every BCOpcode enumerator, so -Wswitch -Werror forces any newly
            // added opcode to be handled here too.
            //==================================================================
            case BCOpcode::TAIL_CALL:
            case BCOpcode::MAKE_ERROR:
            case BCOpcode::OPCODE_COUNT:
                trap(TrapKind::InvalidOpcode, "Unknown opcode");
                break;
        }
    }
}

/// @brief Enter a function frame with arguments already pushed on the operand stack.
/// @details Validates arity and frame footprint, turns the argument slots into
///          locals, initializes remaining locals, and publishes the new frame.
/// @param func Callee in the loaded module.
/// @param site Call path included in validation traps.
void BytecodeVM::enterCallFrame(const BytecodeFunction *func, const char *site) {
    if (!ensureFrameFootprint(func, sp_, site))
        return;

    // Save call site PC
    uint32_t callSitePc = fp_ ? fp_->pc - 1 : 0;

    // Arguments are already on stack - they become first N locals
    BCSlot *localsStart = sp_ - func->numParams;

    // Push new frame
    callStack_.push_back({});
    BCFrame &frame = callStack_.back();
    frame.func = func;
    frame.pc = 0;
    frame.locals = localsStart;
    frame.stackBase = localsStart + func->numLocals;
    frame.ehStackDepth = static_cast<uint32_t>(ehStack_.size());
    frame.callSitePc = callSitePc;
    frame.allocaBase = allocaTop_; // Save alloca position for cleanup on return

    // Ensure parameter locals own any incoming string handles.
    for (uint32_t i = 0; i < func->numParams; ++i) {
        BCSlot *slot = localsStart + i;
        if (localIsString(frame, i) && slot->ptr) {
            if (!validateStringHandle(slot->ptr, "BytecodeVM::call(param)")) {
                setSlotOwnsString(slot, false);
                continue;
            }
            if (!slotOwnsString(slot))
                rt_str_retain_maybe(static_cast<rt_string>(slot->ptr));
            setSlotOwnsString(slot, true);
        } else {
            setSlotOwnsString(slot, false);
        }
    }

    // Zero non-parameter locals
    std::fill(localsStart + func->numParams, localsStart + func->numLocals, BCSlot{});
    for (uint32_t i = func->numParams; i < func->numLocals; ++i)
        setSlotOwnsString(localsStart + i, false);

    // Update stack pointer past locals
    sp_ = frame.stackBase;

    // Switch to new frame
    fp_ = &callStack_.back();
}

/// @brief Enter a bytecode function using arguments already on the operand stack.
/// @details Validates the maximum call depth and exact arity before delegating
///          frame construction to the shared entry helper. The new frame takes
///          its parameter values from the operand stack, establishes their
///          String ownership, and zero-initializes all non-parameter locals.
///          Failure records a VM trap without publishing a partial frame.
/// @param func Borrowed function to call; its arguments must already be pushed
///        in declaration order.
void BytecodeVM::call(const BytecodeFunction *func) {
    // Check stack overflow
    if (callStack_.size() >= kMaxCallDepth) {
        trap(TrapKind::StackOverflow, "call stack overflow");
        return;
    }
    if (!ensureCallArity(func, fp_, sp_, "BytecodeVM::call"))
        return;
    enterCallFrame(func, "BytecodeVM::call");
}

/// @brief Enter a callback frame while a native runtime call is suspended.
/// @param func Callback whose arguments are already on the operand stack.
/// @details Uses the same frame construction as @ref call but labels failures
///          as re-entrant callback setup errors.
void BytecodeVM::callReentrant(const BytecodeFunction *func) {
    if (callStack_.size() >= kMaxCallDepth) {
        trap(TrapKind::StackOverflow, "call stack overflow");
        return;
    }
    enterCallFrame(func, "BytecodeVM::callReentrant");
}

/// @brief Pop the current call frame and return to the caller.
/// @return true if execution can continue in a parent frame, false if at top level.
///
/// Restores the previous frame's state including stack pointer and alloca
/// position. Any stack-allocated memory from the popped frame is released.
bool BytecodeVM::popFrame() {
    releaseFrameLocals(callStack_.back());

    // Restore alloca stack to the base of the popped frame
    // This releases all alloca'd memory from this function call
    allocaTop_ = callStack_.back().allocaBase;

    // Pop frame
    callStack_.pop_back();

    if (callStack_.empty()) {
        fp_ = nullptr;
        return false;
    }

    // Restore previous frame
    fp_ = &callStack_.back();
    sp_ = fp_->stackBase;

    return true;
}

/// @brief Raise a trap, halting execution with an error.
/// @param kind The type of error that occurred.
/// @param message Human-readable description of the error.
/// @details Captures source-aware diagnostic text, releases transient owned
///          strings, clears frames and handlers, and leaves globals intact.
void BytecodeVM::trap(TrapKind kind, const char *message) {
    if (!(pendingTrapErrorCode_ && trapKind_ == kind))
        currentErrorCode_ = defaultBytecodeTrapErrorCode(kind);
    pendingTrapErrorCode_ = false;
    trapKind_ = kind;
    trapMessage_ = formatTrapMessage(kind, currentErrorCode_, message);
    state_ = VMState::Trapped;
}

/// @brief Check for signed addition overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param result Receives the mathematical sum when representable.
/// @return true if overflow would occur, false if safe.
/// Uses compiler builtins when available for efficiency.
bool BytecodeVM::addOverflow(int64_t a, int64_t b, int64_t &result) {
    const auto semantic = il::semantics::checkedAdd(a, b, il::semantics::IntWidth::I64);
    if (!semantic.ok())
        return true;
    result = semantic.value;
    return false;
}

/// @brief Check for signed subtraction overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param result Receives the mathematical difference when representable.
/// @return true if overflow would occur, false if safe.
bool BytecodeVM::subOverflow(int64_t a, int64_t b, int64_t &result) {
    const auto semantic = il::semantics::checkedSub(a, b, il::semantics::IntWidth::I64);
    if (!semantic.ok())
        return true;
    result = semantic.value;
    return false;
}

/// @brief Check for signed multiplication overflow.
/// @param a Left operand.
/// @param b Right operand.
/// @param result Receives the mathematical product when representable.
/// @return true if overflow would occur, false if safe.
bool BytecodeVM::mulOverflow(int64_t a, int64_t b, int64_t &result) {
    const auto semantic = il::semantics::checkedMul(a, b, il::semantics::IntWidth::I64);
    if (!semantic.ok())
        return true;
    result = semantic.value;
    return false;
}

/// @brief Signed 64-bit division with trap detection.
/// @param a Dividend.
/// @param b Divisor.
/// @param result Receives the quotient on success.
/// @param fault Receives the failure category, or `None` on success.
/// @return true and sets @p result; false and sets @p fault to DivideByZero
///         (b==0) or Overflow (INT64_MIN / -1). Backs SDIV_I64[_CHK].
bool BytecodeVM::safeSignedDiv(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const {
    const auto semantic = il::semantics::signedDiv(a, b, il::semantics::IntWidth::I64);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Unsigned 64-bit division (operands reinterpreted as uint64).
/// @param a Dividend bit pattern.
/// @param b Divisor bit pattern.
/// @param result Receives the quotient bit pattern on success.
/// @param fault Receives `DivideByZero` on failure, or `None` on success.
/// @return false with @p fault = DivideByZero when b==0, else true.
bool BytecodeVM::safeUnsignedDiv(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const {
    const auto semantic = il::semantics::unsignedDiv(a, b);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Signed 64-bit remainder; traps DivideByZero on b==0 and defines the
///        INT64_MIN % -1 corner as 0 (where the hardware op would fault).
/// @param a Dividend.
/// @param b Divisor.
/// @param result Receives the remainder on success.
/// @param fault Receives `DivideByZero` on failure, or `None` on success.
bool BytecodeVM::safeSignedRem(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const {
    const auto semantic = il::semantics::signedRem(a, b, il::semantics::IntWidth::I64);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Unsigned 64-bit remainder; traps DivideByZero on b==0.
/// @param a Dividend bit pattern.
/// @param b Divisor bit pattern.
/// @param result Receives the remainder bit pattern on success.
/// @param fault Receives `DivideByZero` on failure, or `None` on success.
bool BytecodeVM::safeUnsignedRem(int64_t a, int64_t b, int64_t &result, TrapKind &fault) const {
    const auto semantic = il::semantics::unsignedRem(a, b);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Checked signed negation; traps Overflow on -INT64_MIN.
/// @param value Operand to negate.
/// @param result Receives the negated value on success.
/// @param fault Receives `Overflow` on failure, or `None` on success.
/// @return `true` when @p value is safely negated.
bool BytecodeVM::safeNegate(int64_t value, int64_t &result, TrapKind &fault) const {
    const auto semantic = il::semantics::negate(value);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Two's-complement wrapping add (no UB): computes in uint64 and
///        bit-copies back. Backs the non-checked ADD_I64.
/// @param a Left operand.
/// @param b Right operand.
/// @return Wrapped sum.
int64_t BytecodeVM::wrappingAdd(int64_t a, int64_t b) noexcept {
    return il::semantics::wrapAdd(a, b);
}

/// @brief Two's-complement wrapping subtract (no UB).
/// @param a Left operand.
/// @param b Right operand.
/// @return Wrapped difference.
int64_t BytecodeVM::wrappingSub(int64_t a, int64_t b) noexcept {
    return il::semantics::wrapSub(a, b);
}

/// @brief Two's-complement wrapping multiply (no UB).
/// @param a Left operand.
/// @param b Right operand.
/// @return Wrapped product.
int64_t BytecodeVM::wrappingMul(int64_t a, int64_t b) noexcept {
    return il::semantics::wrapMul(a, b);
}

/// @brief Logical left shift with the shift amount masked to 0–63 (defined
///        for any shift; no UB from over-shift).
/// @param value Bit pattern to shift.
/// @param shift Shift count; only its low six bits are used.
/// @return Shifted bit pattern represented as `int64_t`.
int64_t BytecodeVM::wrappingShl(int64_t value, int64_t shift) noexcept {
    return il::semantics::shiftLeft(value, shift);
}

/// @brief Arithmetic (sign-extending) right shift, amount masked to 0–63 and
///        sign replication done manually so behavior is portable/defined.
/// @param value Signed bit pattern to shift.
/// @param shift Shift count; only its low six bits are used.
/// @return Sign-extended shifted value.
int64_t BytecodeVM::arithmeticShr(int64_t value, int64_t shift) noexcept {
    return il::semantics::arithmeticShiftRight(value, shift);
}

/// @brief Convert a double to i64 by truncation toward zero (F64_TO_I64_CHK).
/// @param value Floating-point input.
/// @param result Receives the converted integer on success.
/// @param fault Receives `InvalidCast`, `Overflow`, or `None`.
/// @return false with @p fault = InvalidCast (NaN/Inf) or Overflow (outside
///         [-2^63, 2^63)); otherwise true.
bool BytecodeVM::truncF64ToI64(double value, int64_t &result, TrapKind &fault) noexcept {
    const auto semantic = il::semantics::truncF64ToI64(value);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Convert a double to i64 with round-to-nearest ties-to-even, same
///        InvalidCast/Overflow trapping as @ref truncF64ToI64.
/// @param value Floating-point input.
/// @param result Receives the rounded integer on success.
/// @param fault Receives `InvalidCast`, `Overflow`, or `None`.
/// @return `true` when @p value has an in-range finite result.
bool BytecodeVM::roundF64ToI64(double value, int64_t &result, TrapKind &fault) noexcept {
    const auto semantic = il::semantics::fpToSiRte(value, il::semantics::IntWidth::I64);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Convert a double to u64 (round-to-nearest), returning the bit
///        pattern in @p result. Traps InvalidCast on NaN/Inf or a negative
///        value, Overflow at/above 2^64 (F64_TO_U64_CHK).
/// @param value Floating-point input.
/// @param result Receives the unsigned result's bit pattern.
/// @param fault Receives `InvalidCast`, `Overflow`, or `None`.
/// @return `true` when @p value has an in-range finite result.
bool BytecodeVM::roundF64ToU64Bits(double value, int64_t &result, TrapKind &fault) noexcept {
    const auto semantic = il::semantics::fpToUiRte(value, il::semantics::IntWidth::I64);
    if (!semantic.ok()) {
        fault = detail::toBytecodeTrap(semantic.trap);
        return false;
    }
    result = semantic.value;
    fault = TrapKind::None;
    return true;
}

/// @brief Raise a trap, preferring an in-bytecode handler: try dispatchTrap
///        (transfer to an EH handler block) and, if none catches it, fall
///        back to a hard @ref trap that unwinds the VM.
/// @param kind Trap category.
/// @param message Borrowed diagnostic text copied into VM trap state as needed.
/// @param errorCode Runtime-compatible error code exposed to handlers.
/// @return true if a handler took over (execution continues), else false.
bool BytecodeVM::trapOrDispatch(TrapKind kind, const char *message, int32_t errorCode) {
    if (dispatchTrap(kind, errorCode, message))
        return true;
    trap(kind, message);
    return false;
}

/// @brief Bounds-check a local-slot index against the current frame; on
///        failure raises InvalidOpcode via @ref trapOrDispatch.
/// @param idx Candidate local index.
/// @param site Operation name included in the trap message.
/// @return `true` when a current frame exists and @p idx is valid.
bool BytecodeVM::ensureLocalIndex(uint32_t idx, const char *site) {
    if (fp_ && fp_->func && idx < fp_->func->numLocals)
        return true;
    trapOrDispatch(TrapKind::InvalidOpcode,
                   (std::string(site) + ": local index out of range").c_str());
    return false;
}

/// @brief Validate that a memory access range is usable before touching it.
/// @details External host pointers are still permitted after the low-page/null
///          guard because bytecode can interoperate with runtime objects. VM-owned
///          storage is stricter: accesses overlapping global slots or the alloca
///          arena must fit completely inside the corresponding live range.
/// @param ptr First byte to access.
/// @param bytes Requested width; zero is conservatively treated as one byte.
/// @param site Operation name included in a trap.
/// @return `true` when the range is permitted.
bool BytecodeVM::ensureMemoryAccess(const void *ptr, size_t bytes, const char *site) {
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 4096) {
        trapOrDispatch(TrapKind::NullPointer,
                       (std::string(site) + ": null or invalid memory address").c_str());
        return false;
    }

    const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    const size_t width = bytes == 0 ? 1 : bytes;
    if (width > std::numeric_limits<uintptr_t>::max() - addr) {
        trapOrDispatch(
            TrapKind::Bounds,
            (std::string(site) + ": memory access range overflows address space").c_str());
        return false;
    }
    const uintptr_t end = addr + width;

    if (!globals_.empty()) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(globals_.data());
        const uintptr_t globalsEnd = begin + globals_.size() * sizeof(BCSlot);
        if (addr < globalsEnd && end > begin) {
            if (addr >= begin && end <= globalsEnd)
                return true;
            trapOrDispatch(TrapKind::Bounds,
                           (std::string(site) + ": global memory access crosses bounds").c_str());
            return false;
        }
    }

    if (!allocaBuffer_.empty()) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(allocaBuffer_.data());
        const uintptr_t allocatedEnd = begin + allocaTop_;
        const uintptr_t reservedEnd = begin + allocaBuffer_.size();
        if (addr < reservedEnd && end > begin) {
            if (addr >= begin && end <= allocatedEnd)
                return true;
            trapOrDispatch(TrapKind::Bounds,
                           (std::string(site) + ": alloca address outside live range").c_str());
            return false;
        }
    }

    return true;
}

/// @brief Compute a relative branch target without wrapping unsigned PCs.
/// @details The compiler uses origins that are already at the post-opcode PC for
///          compact branches and at the offset-word PC for raw offset operands.
/// @param func Function whose code bounds apply.
/// @param basePc Signed-offset origin.
/// @param offset Encoded relative displacement.
/// @param target Receives the checked absolute program counter.
/// @param site Operation name included in a trap.
/// @return `true` when arithmetic and code bounds are valid.
bool BytecodeVM::computeRelativeTarget(const BytecodeFunction &func,
                                       uint32_t basePc,
                                       int32_t offset,
                                       uint32_t &target,
                                       const char *site) {
    const int64_t computed = static_cast<int64_t>(basePc) + static_cast<int64_t>(offset);
    if (computed < 0 || computed > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        trapOrDispatch(TrapKind::InvalidOpcode,
                       (std::string(site) + ": relative target under/overflows PC range").c_str());
        return false;
    }
    target = static_cast<uint32_t>(computed);
    if (ensureBranchTarget(func, target, site))
        return true;
    return false;
}

/// @brief Add a signed byte offset to @p base with null and wraparound checks.
/// @param base Base address; null is valid only with a zero offset.
/// @param offset Signed byte displacement.
/// @param result Receives the adjusted address on success.
/// @param site Operation name included in a trap.
/// @return `true` when the address calculation does not wrap.
bool BytecodeVM::addPointerOffset(void *base, int64_t offset, void *&result, const char *site) {
    result = nullptr;
    if (!base) {
        if (offset != 0) {
            trapOrDispatch(TrapKind::NullPointer,
                           (std::string(site) + ": GEP on null pointer").c_str());
            return false;
        }
        return true;
    }

    const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(base);
    uintptr_t adjusted;
    if (offset >= 0) {
        const uint64_t positive = static_cast<uint64_t>(offset);
        if (positive > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max() - baseAddr)) {
            trapOrDispatch(TrapKind::Bounds,
                           (std::string(site) + ": pointer offset overflow").c_str());
            return false;
        }
        adjusted = baseAddr + static_cast<uintptr_t>(positive);
    } else {
        const uint64_t magnitude = offset == std::numeric_limits<int64_t>::min()
                                       ? (uint64_t{1} << 63)
                                       : static_cast<uint64_t>(-offset);
        if (magnitude > static_cast<uint64_t>(baseAddr)) {
            trapOrDispatch(TrapKind::Bounds,
                           (std::string(site) + ": pointer offset underflow").c_str());
            return false;
        }
        adjusted = baseAddr - static_cast<uintptr_t>(magnitude);
    }

    result = reinterpret_cast<void *>(adjusted);
    return true;
}

/// @brief Bump-allocate @p requestedSize bytes (8-byte aligned) from the
///        function-lifetime alloca arena, returning the block in @p ptr.
/// @details Traps DomainError on a negative size and StackOverflow on size
///          overflow or arena exhaustion (16 MiB cap). The arena was reserved
///          at construction so it never reallocates — see the ctor note —
///          keeping previously handed-out alloca pointers valid.
/// @param requestedSize Unaligned byte count requested by bytecode.
/// @param ptr Receives the zero-initialized allocation.
/// @param site Operation name included in a trap.
/// @return `true` when allocation succeeds.
bool BytecodeVM::allocateAlloca(int64_t requestedSize, void *&ptr, const char *site) {
    ptr = nullptr;
    if (requestedSize < 0) {
        trapOrDispatch(TrapKind::DomainError,
                       (std::string(site) + ": negative alloca size").c_str());
        return false;
    }

    constexpr size_t kMaxAllocaBytes = 16u * 1024u * 1024u;
    const uint64_t rawSize = static_cast<uint64_t>(requestedSize);
    if (rawSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - 7u)) {
        trapOrDispatch(TrapKind::StackOverflow,
                       (std::string(site) + ": alloca size overflow").c_str());
        return false;
    }

    const size_t alignedSize = (static_cast<size_t>(rawSize) + 7u) & ~size_t{7u};
    if (alignedSize > kMaxAllocaBytes || allocaTop_ > kMaxAllocaBytes - alignedSize) {
        trapOrDispatch(TrapKind::StackOverflow, "alloca stack overflow");
        return false;
    }

    const size_t needed = allocaTop_ + alignedSize;
    if (needed > allocaBuffer_.size()) {
        size_t newSize = allocaBuffer_.empty() ? 64u * 1024u : allocaBuffer_.size();
        while (newSize < needed && newSize < kMaxAllocaBytes)
            newSize = std::min(newSize * 2u, kMaxAllocaBytes);
        if (newSize < needed) {
            trapOrDispatch(TrapKind::StackOverflow, "alloca stack overflow");
            return false;
        }
        allocaBuffer_.resize(newSize);
    }

    ptr = allocaBuffer_.data() + allocaTop_;
    std::memset(ptr, 0, alignedSize);
    allocaTop_ = needed;
    return true;
}

//==============================================================================
// Source Line Tracking
//==============================================================================

/// @brief Get the source line number at the current execution point.
/// @return The source line number, or 0 if not available.
uint32_t BytecodeVM::currentSourceLine() const {
    if (!fp_ || !fp_->func)
        return 0;
    uint32_t pc = state_ == VMState::Trapped ? currentFaultPc() : fp_->pc;
    return getSourceLine(fp_->func, pc);
}

/// @brief Get the source line number for a specific PC in a function.
/// @param func The bytecode function.
/// @param pc The program counter offset.
/// @return The source line number, or 0 if not available.
///
/// Uses the function's line table to map bytecode offsets back to
/// source locations for debugging and error reporting.
uint32_t BytecodeVM::getSourceLine(const BytecodeFunction *func, uint32_t pc) {
    if (!func || func->lineTable.empty())
        return 0;
    if (pc >= func->lineTable.size())
        return 0;
    return func->lineTable[pc];
}

/// @brief PC of the faulting instruction (pc-1, since the fetch already
///        advanced past it); 0 if there is no frame.
uint32_t BytecodeVM::currentFaultPc() const {
    return (fp_ && fp_->pc > 0) ? (fp_->pc - 1) : 0;
}

/// @brief Block label associated with @p pc via the function's label table
///        (empty if unavailable) — used in trap diagnostics.
/// @param func Function containing @p pc.
/// @param pc Program counter to resolve.
/// @return Copied block label, or an empty string.
std::string BytecodeVM::currentBlockLabelForPc(const BytecodeFunction *func, uint32_t pc) const {
    if (!func || pc >= func->blockLabelTable.size())
        return {};
    return func->blockLabelTable[pc];
}

/// @brief Resolve the source file path for @p pc from the function's
///        source-file table (empty if unknown) — used in trap diagnostics.
/// @param func Function containing @p pc.
/// @param pc Program counter to resolve.
/// @return Copied source path, or an empty string.
std::string BytecodeVM::currentSourcePathForPc(const BytecodeFunction *func, uint32_t pc) const {
    if (!module_ || !func)
        return {};

    uint32_t sourceFileEntry = 0;
    if (pc < func->sourceFileTable.size())
        sourceFileEntry = func->sourceFileTable[pc];

    if (sourceFileEntry != 0) {
        const uint32_t sourceIndex = sourceFileEntry - 1;
        if (sourceIndex < module_->sourceFiles.size())
            return module_->sourceFiles[sourceIndex].path;
    }

    if (func->sourceFileIdx < module_->sourceFiles.size())
        return module_->sourceFiles[func->sourceFileIdx].path;

    return {};
}

/// @brief Build the human-readable trap report string: trap kind name,
///        optional error code and message, and the faulting function /
///        block / source location resolved from the current frame.
/// @param kind Trap category to format.
/// @param errorCode Runtime-compatible numeric code.
/// @param message Optional detail text.
/// @return Complete diagnostic string suitable for `trapMessage_`.
std::string BytecodeVM::formatTrapMessage(TrapKind kind,
                                          int32_t errorCode,
                                          const char *message) const {
    std::ostringstream out;
    out << "Trap";

    if (fp_ && fp_->func) {
        const BytecodeFunction *func = fp_->func;
        const uint32_t pc = currentFaultPc();
        out << " @" << func->name;

        const std::string blockLabel = currentBlockLabelForPc(func, pc);
        if (!blockLabel.empty())
            out << ':' << blockLabel;

        out << '#' << pc;

        const std::string sourcePath = currentSourcePathForPc(func, pc);
        const uint32_t sourceLine = getSourceLine(func, pc);
        if (!sourcePath.empty() || sourceLine != 0) {
            out << " (";
            if (!sourcePath.empty())
                out << sourcePath;
            if (sourceLine != 0) {
                if (!sourcePath.empty())
                    out << ':';
                out << sourceLine;
            }
            out << ')';
        }
    }

    out << ": " << bytecodeTrapKindName(kind) << " (code=" << errorCode << ')';
    if (message && *message)
        out << ": " << message;
    return out.str();
}

//==============================================================================
// Exception Handling
//==============================================================================

/// @brief Push an exception handler onto the handler stack.
/// @param handlerPc The program counter of the handler entry point.
///
/// Captures the current frame index and stack pointer so the VM can unwind
/// to this state if a trap occurs within the protected region.
void BytecodeVM::pushExceptionHandler(uint32_t handlerPc) {
    BCExceptionHandler eh;
    eh.handlerPc = handlerPc;
    eh.frameIndex = static_cast<uint32_t>(callStack_.size() - 1);
    eh.stackPointer = sp_;
    ehStack_.push_back(eh);
}

/// @brief Pop the most recently pushed exception handler.
///
/// Called when exiting a protected region normally (no exception occurred).
void BytecodeVM::popExceptionHandler() {
    if (!ehStack_.empty()) {
        ehStack_.pop_back();
    }
}

/// @brief Dispatch a trap to the nearest exception handler.
/// @param kind The type of trap that occurred.
/// @return true if a handler was found and jumped to, false if no handler exists.
///
/// Unwinds the call stack searching for a registered exception handler.
/// If found, restores the stack to the handler's saved state, pushes
/// error information onto the operand stack, and transfers control
/// to the handler. Returns false if the trap propagates to the top level.
/// @param kind Trap category exposed to the handler.
/// @param errorCode Numeric error code exposed to `ERR_GET_CODE`.
/// @param message Optional diagnostic text captured in the trap record.
/// @return `true` when a matching live handler receives control.
bool BytecodeVM::dispatchTrap(TrapKind kind, int32_t errorCode, const char *message) {
    clearTrapRecord();
    trapKind_ = kind;
    currentErrorCode_ = errorCode >= 0 ? errorCode : defaultBytecodeTrapErrorCode(kind);
    pendingTrapErrorCode_ = true;
    if (message)
        trapMessage_ = formatTrapMessage(kind, currentErrorCode_, message);

    // Search for a handler and auto-pop it from the EH stack on dispatch.
    // Handler blocks always start with a clean EH stack — if catch body throws,
    // the trap propagates to the next outer handler rather than re-entering
    // the same one. Normal-path cleanup uses explicit eh.pop in IL.
    while (!ehStack_.empty()) {
        BCExceptionHandler eh = ehStack_.back();
        ehStack_.pop_back();
        if (eh.frameIndex >= callStack_.size())
            continue;

        clearTrapRecord();
        trapRecord_.valid = true;
        trapRecord_.kind = kind;
        trapRecord_.errorCode = errorCode;
        trapRecord_.faultPc = (fp_ && fp_->pc > 0) ? (fp_->pc - 1) : 0;
        trapRecord_.nextPc = fp_ ? fp_->pc : 0;
        const uint32_t line =
            (fp_ && fp_->func) ? getSourceLine(fp_->func, trapRecord_.faultPc) : 0;
        trapRecord_.faultLine = line ? static_cast<int32_t>(line) : -1;
        trapRecord_.valueCount = static_cast<size_t>(sp_ - valueStack_.data());
        trapRecord_.stackPointerIndex = trapRecord_.valueCount;
        trapRecord_.resumeStackPointerIndex =
            static_cast<size_t>(eh.stackPointer - valueStack_.data());
        trapRecord_.valueSlots.assign(valueStack_.begin(),
                                      valueStack_.begin() + trapRecord_.valueCount);
        trapRecord_.valueOwned = valueStackStringOwned_.snapshotPrefix(trapRecord_.valueCount);
        for (size_t i = 0; i < trapRecord_.valueCount; ++i) {
            if (trapRecord_.valueOwned[i] == 0 || !trapRecord_.valueSlots[i].ptr)
                continue;
            if (validateStringHandle(trapRecord_.valueSlots[i].ptr,
                                     "BytecodeVM::dispatchTrap(snapshot)")) {
                rt_str_retain_maybe(static_cast<rt_string>(trapRecord_.valueSlots[i].ptr));
            }
        }
        trapRecord_.callStack = callStack_;
        trapRecord_.ehStack = ehStack_;
        trapRecord_.allocaSize = allocaTop_;
        trapRecord_.allocaBytes.assign(allocaBuffer_.begin(), allocaBuffer_.begin() + allocaTop_);

        // Unwind call stack to the frame where handler was registered
        while (callStack_.size() > eh.frameIndex + 1) {
            BCFrame &unwound = callStack_.back();
            while (sp_ > unwound.stackBase)
                releaseOwnedString(--sp_);
            releaseFrameLocals(unwound);
            allocaTop_ = unwound.allocaBase;
            callStack_.pop_back();
            if (!callStack_.empty())
                sp_ = callStack_.back().stackBase;
        }

        if (!callStack_.empty()) {
            fp_ = &callStack_.back();
            while (sp_ > eh.stackPointer)
                releaseOwnedString(--sp_);
            sp_ = eh.stackPointer;

            // Store trap info for err.get_* introspection
            trapKind_ = kind;
            currentErrorCode_ = errorCode >= 0 ? errorCode : defaultBytecodeTrapErrorCode(kind);
            pendingTrapErrorCode_ = false;

            // Push trap kind onto stack for handler to inspect (as error token)
            sp_->i64 = static_cast<int64_t>(kind);
            setSlotOwnsString(sp_, false);
            sp_++;
            // Push an opaque resume token pointing at the retained trap record.
            sp_->ptr = &trapRecord_;
            setSlotOwnsString(sp_, false);
            sp_++;

            // Jump to handler
            fp_->pc = eh.handlerPc;
            state_ = VMState::Running;
            return true;
        }

        // Frame for this handler no longer exists — already popped above, try next
        clearTrapRecord();
    }

    // No handler found - trap propagates to top level
    return false;
}

/// @brief Implement the RESUME_* opcodes: validate the resume token against
///        the saved trap record, restore the captured value stack / call /
///        EH state, and continue at the faulting instruction (@p useNextPc
///        false) or the one after it (RESUME_NEXT).
/// @param useNextPc Selects the post-fault PC rather than the faulting PC.
/// @return false if the token does not match a valid trap record (the caller
///         then treats it as a hard error).
bool BytecodeVM::resumeTrap(bool useNextPc) {
    BCSlot token = *--sp_;
    setSlotOwnsString(sp_, false);
    if (token.ptr != &trapRecord_ || !trapRecord_.valid)
        return false;

    releaseOwnedValueStack();
    callStack_.clear();
    ehStack_.clear();
    valueStackStringOwned_.clearAll();

    const size_t restoredCount =
        useNextPc ? trapRecord_.resumeStackPointerIndex : trapRecord_.stackPointerIndex;
    if (trapRecord_.valueCount > valueStack_.size() || restoredCount > trapRecord_.valueCount)
        return false;
    valueStackStringOwned_.clearAll();
    std::copy(trapRecord_.valueSlots.begin(),
              trapRecord_.valueSlots.begin() + restoredCount,
              valueStack_.begin());
    valueStackStringOwned_.restorePrefix(trapRecord_.valueOwned, restoredCount);
    sp_ = valueStack_.data() + restoredCount;

    callStack_ = trapRecord_.callStack;
    ehStack_ = trapRecord_.ehStack;
    fp_ = callStack_.empty() ? nullptr : &callStack_.back();

    if (trapRecord_.allocaSize > allocaBuffer_.size())
        allocaBuffer_.resize(trapRecord_.allocaSize);
    std::copy(
        trapRecord_.allocaBytes.begin(), trapRecord_.allocaBytes.end(), allocaBuffer_.begin());
    allocaTop_ = trapRecord_.allocaSize;

    if (!fp_ || !fp_->func)
        return false;
    fp_->pc = useNextPc ? trapRecord_.nextPc : trapRecord_.faultPc;
    state_ = VMState::Running;
    for (size_t i = 0; i < restoredCount && i < trapRecord_.valueOwned.size(); ++i)
        trapRecord_.valueOwned[i] = 0;
    clearTrapRecord();
    return true;
}

//==============================================================================
// Debug Support
//==============================================================================

/// @brief Set a breakpoint at a specific location.
/// @param funcName The name of the function containing the breakpoint.
/// @param pc The program counter offset within the function.
void BytecodeVM::setBreakpoint(const std::string &funcName, uint32_t pc) {
    breakpoints_[funcName].insert(pc);
}

/// @brief Clear a breakpoint at a specific location.
/// @param funcName The name of the function containing the breakpoint.
/// @param pc The program counter offset to clear.
void BytecodeVM::clearBreakpoint(const std::string &funcName, uint32_t pc) {
    auto it = breakpoints_.find(funcName);
    if (it != breakpoints_.end()) {
        it->second.erase(pc);
        if (it->second.empty()) {
            breakpoints_.erase(it);
        }
    }
}

/// @brief Clear all breakpoints in all functions.
/// @post `breakpoints_` is empty.
void BytecodeVM::clearAllBreakpoints() {
    breakpoints_.clear();
}

/// @brief Check if execution should pause at the current location.
/// @return true if execution should pause (breakpoint hit or single-stepping).
///
/// Called at the start of each instruction. Invokes the debug callback
/// if a breakpoint is hit or single-step mode is enabled.
bool BytecodeVM::checkBreakpoint() {
    if (!fp_ || !fp_->func)
        return false;

    bool isBreakpoint = false;
    auto it = breakpoints_.find(fp_->func->name);
    if (it != breakpoints_.end()) {
        isBreakpoint = it->second.count(fp_->pc) > 0;
    }

    // Check if we should pause (breakpoint hit or single-stepping)
    if (isBreakpoint || singleStep_) {
        return requestDebugPause(isBreakpoint, fp_->pc);
    }
    return false;
}

/// @brief Invoke the debugger callback for a pause event.
/// @details The callback contract returns true to continue and false to pause.
///          Without a callback, a breakpoint or single-step request pauses by
///          default; VMState::Halted is used as the current pause state.
/// @param isBreakpoint Whether the pause was caused by a registered breakpoint.
/// @param pc Program counter reported to the debugger.
/// @return `true` when execution should pause.
bool BytecodeVM::requestDebugPause(bool isBreakpoint, uint32_t pc) {
    if (!fp_ || !fp_->func)
        return false;
    if (debugCallback_)
        return !debugCallback_(*this, fp_->func, pc, isBreakpoint);
    return true;
}

//===----------------------------------------------------------------------===//
// Bytecode VM Thread.Start Handler
//===----------------------------------------------------------------------===//

namespace {

/// @brief Self-contained state for executing a bytecode entry on a worker thread.
/// @details `moduleOwner` keeps the snapshot backing `module` and `entry`
///          alive; the worker consumes and deletes the payload.
struct BytecodeThreadPayload {
    std::shared_ptr<const BytecodeModule> moduleOwner{};
    const BytecodeModule *module = nullptr;
    const BytecodeFunction *entry = nullptr;
    void *arg = nullptr;
    bool ownsArg = false;
    BytecodeVM::ExecutionEnvironment environment;
};

/// @brief Self-contained state for executing `Async.Run` on a bytecode worker.
/// @details The worker settles `promise`, releases transferred resources, and
///          deletes the payload on every completion path.
struct BytecodeAsyncPayload {
    std::shared_ptr<const BytecodeModule> moduleOwner{};
    const BytecodeModule *module = nullptr;
    const BytecodeFunction *entry = nullptr;
    void *arg = nullptr;
    bool ownsArg = false;
    BytecodeVM::ExecutionEnvironment environment;
    void *promise = nullptr;
};

/// @brief Drop a worker thread/async payload's owned argument object (no-op
///        if not owned or null).
/// @param arg Runtime object passed to the worker.
/// @param ownsArg Whether the worker owns a reference to @p arg.
static void releaseWorkerArg(void *arg, bool ownsArg) {
    if (!ownsArg || !arg)
        return;
    if (rt_obj_release_check0(arg))
        rt_obj_free(arg);
}

/// @brief Settle an Async.Run promise with a successful @p result.
/// @details If the worker returned its own owned argument object, ownership is
///          transferred to the promise (no double free); otherwise the owned
///          argument is released and the result is set as owned.
/// @param promise Promise to settle; must be valid.
/// @param result Worker result object, possibly identical to `*ownedArg`.
/// @param ownedArg Address of the payload's argument pointer.
/// @param ownsArg Address of the payload's ownership flag.
static void completeAsyncPromiseWithResult(void *promise,
                                           void *result,
                                           void **ownedArg,
                                           bool *ownsArg) {
    if (ownedArg && ownsArg && *ownsArg) {
        if (result == *ownedArg) {
            *ownedArg = nullptr;
            *ownsArg = false;
            rt_promise_set_transferred(promise, result);
            return;
        }
        releaseWorkerArg(*ownedArg, true);
        *ownedArg = nullptr;
        *ownsArg = false;
    }
    rt_promise_set_owned(promise, result);
}

/// @brief Settle an Async.Run promise with an @p error, first releasing any
///        owned worker argument so a failed task cannot leak it.
/// @param promise Promise to reject; must be valid.
/// @param error Runtime string passed to the promise error channel.
/// @param ownedArg Address of the payload's argument pointer.
/// @param ownsArg Address of the payload's ownership flag.
static void completeAsyncPromiseWithError(void *promise,
                                          rt_string error,
                                          void **ownedArg,
                                          bool *ownsArg) {
    if (ownedArg && ownsArg && *ownsArg) {
        releaseWorkerArg(*ownedArg, true);
        *ownedArg = nullptr;
        *ownsArg = false;
    }
    rt_promise_set_error(promise, error);
}

/// @brief Publish or release an Async.Run future returned by a runtime shim.
/// @details Runtime calls normally provide @p result for return values. If a
///          malformed/direct call path invokes the shim with no result slot,
///          the future must be released here because no caller can receive it.
/// @param result Optional runtime result slot.
/// @param future Future object returned by the promise.
static void publishAsyncFutureResult(void *result, void *future) {
    if (result) {
        *reinterpret_cast<void **>(result) = future;
        return;
    }
    if (future && rt_obj_release_check0(future))
        rt_obj_free(future);
}

/// @brief Run a spawned bytecode thread's entry function on a fresh worker VM.
/// @details Builds a worker BytecodeVM, mirrors the parent's execution
///          environment, installs the active-VM/module thread context, and
///          invokes @c payload->entry with its argument. On failure writes a
///          message into @p errorBuf.
/// @param payload Owned worker payload; deleted before return.
/// @param errorBuf Optional output buffer for a null-terminated failure message.
/// @param errorBufSize Capacity of @p errorBuf in bytes.
/// @return true on clean completion, false on error (message in @p errorBuf).
static bool runBytecodeThreadPayload(BytecodeThreadPayload *payload,
                                     char *errorBuf,
                                     size_t errorBufSize) {
    if (errorBuf && errorBufSize > 0)
        errorBuf[0] = '\0';
    if (payload && payload->moduleOwner)
        payload->module = payload->moduleOwner.get();
    if (!payload || !payload->module || !payload->entry) {
        if (errorBuf && errorBufSize > 0)
            std::snprintf(errorBuf, errorBufSize, "%s", "Thread.StartSafe: invalid bytecode entry");
        if (payload)
            releaseWorkerArg(payload->arg, payload->ownsArg);
        delete payload;
        return false;
    }

    BytecodeVM vm;
    vm.load(payload->module);
    vm.applyExecutionEnvironment(payload->environment);

    std::vector<BCSlot> args;
    if (payload->entry->numParams > 0) {
        BCSlot argSlot{};
        argSlot.ptr = payload->arg;
        args.push_back(argSlot);
    }

    vm.exec(payload->entry, args);
    if (vm.state() == VMState::Trapped) {
        const std::string &message = vm.trapMessage();
        if (errorBuf && errorBufSize > 0) {
            std::snprintf(errorBuf,
                          errorBufSize,
                          "%s",
                          message.empty() ? "Thread.StartSafe: trapped bytecode worker"
                                          : message.c_str());
        }
        releaseWorkerArg(payload->arg, payload->ownsArg);
        delete payload;
        return false;
    }

    releaseWorkerArg(payload->arg, payload->ownsArg);
    delete payload;
    return true;
}

/// @brief C ABI trampoline for an ordinary bytecode-backed thread.
/// @param raw Owned `BytecodeThreadPayload`; consumed by the worker.
/// @details Aborts the runtime if bytecode setup or execution traps.
extern "C" void bytecode_thread_entry_trampoline(void *raw) {
    char error[512];
    if (!runBytecodeThreadPayload(static_cast<BytecodeThreadPayload *>(raw), error, sizeof(error)))
        rt_abort(error[0] ? error : "Thread.Start: trapped bytecode worker");
}

/// @brief C ABI trampoline for a trap-recoverable bytecode-backed thread.
/// @param raw Owned `BytecodeThreadPayload`; consumed by the worker.
/// @details Avoids live C++ objects across the runtime's `setjmp`/`longjmp`
///          recovery boundary and re-raises failures through `rt_trap`.
extern "C" void bytecode_thread_safe_entry_trampoline(void *raw) {
    // rt_thread_start_safe uses setjmp/longjmp for recovery, so this trampoline
    // must not hold live C++ objects across rt_trap().
    char error[512];
    if (!runBytecodeThreadPayload(static_cast<BytecodeThreadPayload *>(raw), error, sizeof(error)))
        rt_trap(error[0] ? error : "Thread.StartSafe: trapped bytecode worker");
}

/// @brief Resolve a tagged or legacy raw bytecode function pointer.
/// @details Tagged pointers use the high bit as a discriminator and store the
///          function-table index in the remaining bits. Raw table-element
///          pointers are accepted for compatibility.
/// @param module Module whose function table owns the entry.
/// @param entry Opaque runtime function value.
/// @return Borrowed function pointer, or null when @p entry is invalid.
static const BytecodeFunction *resolveBytecodeEntry(const BytecodeModule *module, void *entry) {
    if (!entry || !module)
        return nullptr;

    // Check if this is a tagged function pointer (high bit set)
    constexpr uint64_t kFuncPtrTag = 0x8000000000000000ULL;
    uint64_t val = reinterpret_cast<uint64_t>(entry);

    if (val & kFuncPtrTag) {
        // Extract function index from tagged pointer
        uint64_t funcIdx = val & ~kFuncPtrTag;
        if (funcIdx < module->functions.size()) {
            return &module->functions[funcIdx];
        }
        return nullptr;
    }

    // Fallback: try to match as a raw pointer (for compatibility)
    const auto *candidate = static_cast<const BytecodeFunction *>(entry);
    for (const auto &fn : module->functions) {
        if (&fn == candidate) {
            return &fn;
        }
    }
    return nullptr;
}

/// @brief Owned state passed to a tree-walker VM thread trampoline.
struct VmThreadStartPayload {
    const il::core::Module *module = nullptr;
    std::shared_ptr<il::vm::VM::ProgramState> program;
    il::vm::ExternRegistry *externRegistry = nullptr;
    const il::core::Function *entry = nullptr;
    void *arg = nullptr;
    bool ownsArg = false;
};

/// @brief Owned state passed to a tree-walker `Async.Run` trampoline.
struct VmAsyncRunPayload {
    const il::core::Module *module = nullptr;
    std::shared_ptr<il::vm::VM::ProgramState> program;
    il::vm::ExternRegistry *externRegistry = nullptr;
    const il::core::Function *entry = nullptr;
    void *arg = nullptr;
    bool ownsArg = false;
    void *promise = nullptr;
};

/// @brief Persistent tree-walker callback state registered with an HTTP server.
struct VmHttpHandlerPayload {
    const il::core::Module *module = nullptr;
    std::shared_ptr<il::vm::VM::ProgramState> program;
    il::vm::ExternRegistry *externRegistry = nullptr;
    const il::core::Function *entry = nullptr;
};

/// @brief Persistent bytecode callback state registered with an HTTP server.
struct BytecodeHttpHandlerPayload {
    std::shared_ptr<const BytecodeModule> moduleOwner{};
    const BytecodeModule *module = nullptr;
    const BytecodeFunction *entry = nullptr;
    BytecodeVM::ExecutionEnvironment environment;
};

/// @brief Clone a bytecode module for work that may outlive the caller VM.
/// @details Runtime payloads for threads, async tasks, and HTTP handlers cannot
///          safely borrow the active module because many CLI paths keep it on
///          the stack. A shared snapshot gives those payloads stable function
///          and constant-pool storage.
/// @param module Active bytecode module to snapshot.
/// @return Shared immutable snapshot, or empty on allocation failure/null input.
static std::shared_ptr<const BytecodeModule> cloneBytecodeModuleForWorker(
    const BytecodeModule *module) noexcept {
    if (!module)
        return {};
    try {
        return std::make_shared<BytecodeModule>(*module);
    } catch (...) {
        return {};
    }
}

/// @brief Resolve an entry function inside a cloned bytecode module.
/// @details Function pointers from the caller module cannot be used with a
///          cloned module, so payload construction re-resolves by stable
///          function name.
/// @param module Cloned module snapshot.
/// @param originalEntry Entry function resolved in the caller module.
/// @return Matching function in @p module, or null when unavailable.
static const BytecodeFunction *resolveClonedBytecodeEntry(
    const std::shared_ptr<const BytecodeModule> &module, const BytecodeFunction *originalEntry) {
    if (!module || !originalEntry)
        return nullptr;
    return module->findFunction(originalEntry->name);
}

/// @brief C ABI trampoline that executes a tree-walker IL thread entry.
/// @param raw Owned `VmThreadStartPayload`; always released before return.
/// @details Converts VM traps and C++ exceptions to runtime aborts after
///          releasing the argument and retained external-function registry.
extern "C" void vm_thread_entry_trampoline_bc(void *raw) {
    VmThreadStartPayload *payload = static_cast<VmThreadStartPayload *>(raw);
    if (!payload || !payload->module || !payload->entry) {
        if (payload) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
        }
        delete payload;
        rt_abort("Thread.Start: invalid entry");
        return;
    }

    try {
        il::vm::VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);
        il::support::SmallVector<il::vm::Slot, 2> args;
        if (payload->entry->params.size() == 1) {
            il::vm::Slot s{};
            s.ptr = payload->arg;
            args.push_back(s);
        }
        il::vm::detail::VMAccess::callFunction(vm, *payload->entry, args);
    } catch (const il::vm::RuntimeTrapSignal &signal) {
        char error[512];
        const char *message =
            signal.message.empty() ? "Thread.Start: trapped VM worker" : signal.message.c_str();
        std::snprintf(error, sizeof(error), "%s", message);
        releaseWorkerArg(payload->arg, payload->ownsArg);
        il::vm::releaseExternRegistry(payload->externRegistry);
        payload->externRegistry = nullptr;
        delete payload;
        rt_abort(error);
        return;
    } catch (const std::exception &ex) {
        char error[512];
        std::snprintf(error, sizeof(error), "Thread.Start: unhandled exception: %s", ex.what());
        releaseWorkerArg(payload->arg, payload->ownsArg);
        il::vm::releaseExternRegistry(payload->externRegistry);
        payload->externRegistry = nullptr;
        delete payload;
        rt_abort(error);
        return;
    } catch (...) {
        releaseWorkerArg(payload->arg, payload->ownsArg);
        il::vm::releaseExternRegistry(payload->externRegistry);
        payload->externRegistry = nullptr;
        delete payload;
        rt_abort("Thread.Start: unhandled non-standard exception");
        return;
    }
    releaseWorkerArg(payload->arg, payload->ownsArg);
    il::vm::releaseExternRegistry(payload->externRegistry);
    delete payload;
}

/// @brief Trap-recoverable C ABI trampoline for a tree-walker IL thread entry.
/// @param raw Owned `VmThreadStartPayload`; always released before return.
/// @details Converts VM traps and C++ exceptions to `rt_trap`, allowing the
///          safe thread runtime to capture them.
extern "C" void vm_thread_safe_entry_trampoline_bc(void *raw) {
    VmThreadStartPayload *payload = static_cast<VmThreadStartPayload *>(raw);
    if (!payload || !payload->module || !payload->entry) {
        if (payload) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
        }
        delete payload;
        rt_trap("Thread.StartSafe: invalid entry");
        return;
    }

    try {
        il::vm::VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);
        il::support::SmallVector<il::vm::Slot, 2> args;
        if (payload->entry->params.size() == 1) {
            il::vm::Slot s{};
            s.ptr = payload->arg;
            args.push_back(s);
        }
        il::vm::detail::VMAccess::callFunction(vm, *payload->entry, args);
    } catch (const il::vm::RuntimeTrapSignal &signal) {
        char error[512];
        const char *message =
            signal.message.empty() ? "Thread.StartSafe: trapped VM worker" : signal.message.c_str();
        std::snprintf(error, sizeof(error), "%s", message);
        releaseWorkerArg(payload->arg, payload->ownsArg);
        il::vm::releaseExternRegistry(payload->externRegistry);
        payload->externRegistry = nullptr;
        delete payload;
        rt_trap(error);
        return;
    } catch (const std::exception &ex) {
        char error[512];
        std::snprintf(error, sizeof(error), "Thread.StartSafe: unhandled exception: %s", ex.what());
        releaseWorkerArg(payload->arg, payload->ownsArg);
        il::vm::releaseExternRegistry(payload->externRegistry);
        payload->externRegistry = nullptr;
        delete payload;
        rt_trap(error);
        return;
    } catch (...) {
        releaseWorkerArg(payload->arg, payload->ownsArg);
        il::vm::releaseExternRegistry(payload->externRegistry);
        payload->externRegistry = nullptr;
        delete payload;
        rt_trap("Thread.StartSafe: unhandled non-standard exception");
        return;
    }
    releaseWorkerArg(payload->arg, payload->ownsArg);
    il::vm::releaseExternRegistry(payload->externRegistry);
    delete payload;
}

/// @brief Resolve an opaque raw pointer to an IL function owned by @p module.
/// @param module Module whose function table is searched.
/// @param entry Candidate raw function address.
/// @return Borrowed matching function, or null.
static const il::core::Function *resolveILEntry(const il::core::Module &module, void *entry) {
    if (!entry)
        return nullptr;
    const auto *candidate = static_cast<const il::core::Function *>(entry);
    for (const auto &fn : module.functions) {
        if (&fn == candidate)
            return &fn;
    }
    return nullptr;
}

/// @brief Validate the tree-walker `Thread.Start` callback signature.
/// @param fn Candidate entry function.
/// @details Traps unless @p fn returns `Unit` and accepts zero parameters or
///          one pointer parameter.
static void validateEntrySignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Void) {
        rt_trap("Thread.Start: invalid entry signature");
        return;
    }
    if (fn.params.empty())
        return;
    if (fn.params.size() == 1 && fn.params[0].type.kind == Kind::Ptr)
        return;
    rt_trap("Thread.Start: invalid entry signature");
    return;
}

/// @brief Trap unless IL function @p fn matches the Async.Run entry shape
///        (ptr return, single ptr parameter).
/// @param fn Candidate tree-walker callback.
static void validateAsyncEntrySignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Ptr) {
        rt_trap("Async.Run: invalid entry signature");
        return;
    }
    if (fn.params.size() == 1 && fn.params[0].type.kind == Kind::Ptr)
        return;
    rt_trap("Async.Run: invalid entry signature");
    return;
}

/// @brief Trap unless IL function @p fn matches the HttpServer.BindHandler
///        entry shape (void return, two ptr parameters).
/// @param fn Candidate tree-walker callback.
static void validateHttpHandlerSignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Void || fn.params.size() != 2 ||
        fn.params[0].type.kind != Kind::Ptr || fn.params[1].type.kind != Kind::Ptr) {
        rt_trap("HttpServer.BindHandler: invalid entry signature");
        return;
    }
}

/// @brief Trap unless IL function @p fn matches the HttpsServer.BindHandler
///        entry shape (void return, two ptr parameters).
/// @param fn Candidate tree-walker callback.
static void validateHttpsHandlerSignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Void || fn.params.size() != 2 ||
        fn.params[0].type.kind != Kind::Ptr || fn.params[1].type.kind != Kind::Ptr) {
        rt_trap("HttpsServer.BindHandler: invalid entry signature");
        return;
    }
}

/// @brief Trap unless bytecode function @p fn is a valid Thread.Start entry
///        (no return value; zero or one parameter).
/// @param fn Candidate bytecode callback.
static void validateBytecodeThreadEntrySignature(const BytecodeFunction &fn) {
    if (fn.hasReturn) {
        rt_trap("Thread.Start: invalid bytecode entry signature");
        return;
    }
    if (fn.numParams == 0 || fn.numParams == 1)
        return;
    rt_trap("Thread.Start: invalid bytecode entry signature");
    return;
}

/// @brief Trap unless bytecode function @p fn is a valid Async.Run entry
///        (returns a value; exactly one parameter).
/// @param fn Candidate bytecode callback.
static void validateBytecodeAsyncEntrySignature(const BytecodeFunction &fn) {
    if (!fn.hasReturn || fn.numParams != 1) {
        rt_trap("Async.Run: invalid bytecode entry signature");
        return;
    }
}

/// @brief Trap unless bytecode function @p fn is a valid HttpServer
///        BindHandler entry (no return value; exactly two parameters).
/// @param fn Candidate bytecode callback.
static void validateBytecodeHttpHandlerSignature(const BytecodeFunction &fn) {
    if (fn.hasReturn || fn.numParams != 2) {
        rt_trap("HttpServer.BindHandler: invalid bytecode entry signature");
        return;
    }
}

/// @brief Trap unless bytecode function @p fn is a valid HttpsServer
///        BindHandler entry (no return value; exactly two parameters).
/// @param fn Candidate bytecode callback.
static void validateBytecodeHttpsHandlerSignature(const BytecodeFunction &fn) {
    if (fn.hasReturn || fn.numParams != 2) {
        rt_trap("HttpsServer.BindHandler: invalid bytecode entry signature");
        return;
    }
}

/// @brief Thread-local dispatch context used by native Game3D callback trampolines.
/// @details Exactly one VM/function pair is populated for each callback kind.
///          `previous` permits nested game loops to restore an outer context.
struct UnifiedGame3DCallbackScope {
    il::vm::VM *stdVm = nullptr;
    BytecodeVM *bcVm = nullptr;
    const il::core::Function *stdUpdate = nullptr;
    const il::core::Function *stdOverlay = nullptr;
    const BytecodeFunction *bcUpdate = nullptr;
    const BytecodeFunction *bcOverlay = nullptr;
    UnifiedGame3DCallbackScope *previous = nullptr;
};

/// @brief Active Game3D callback context on the current thread.
thread_local UnifiedGame3DCallbackScope *tlsUnifiedGame3DScope = nullptr;

/// @brief Validate a tree-walker Game3D update callback.
/// @param fn Candidate callback.
/// @param api Runtime API name prefixed to the trap message.
/// @details Traps unless the signature is `(Float) -> Unit`.
static void validateGame3DUpdateSignature(const il::core::Function &fn, const char *api) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind == Kind::Void && fn.params.size() == 1 &&
        fn.params[0].type.kind == Kind::F64) {
        return;
    }
    std::string message(api);
    message += ": update callback must have signature (Float) -> Unit";
    rt_trap(message.c_str());
}

/// @brief Validate a tree-walker Game3D overlay callback.
/// @param fn Candidate callback.
/// @param api Runtime API name prefixed to the trap message.
/// @details Traps unless the signature is `() -> Unit`.
static void validateGame3DOverlaySignature(const il::core::Function &fn, const char *api) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind == Kind::Void && fn.params.empty())
        return;
    std::string message(api);
    message += ": overlay callback must have signature () -> Unit";
    rt_trap(message.c_str());
}

/// @brief Validate a bytecode Game3D update callback.
/// @param fn Candidate callback.
/// @param api Runtime API name prefixed to the trap message.
/// @details Bytecode stores only arity and return-presence metadata, so this
///          enforces one parameter and no return value.
static void validateBytecodeGame3DUpdateSignature(const BytecodeFunction &fn, const char *api) {
    if (!fn.hasReturn && fn.numParams == 1)
        return;
    std::string message(api);
    message += ": update callback must have signature (Float) -> Unit";
    rt_trap(message.c_str());
}

/// @brief Validate a bytecode Game3D overlay callback.
/// @param fn Candidate callback.
/// @param api Runtime API name prefixed to the trap message.
/// @details Requires zero parameters and no return value.
static void validateBytecodeGame3DOverlaySignature(const BytecodeFunction &fn, const char *api) {
    if (!fn.hasReturn && fn.numParams == 0)
        return;
    std::string message(api);
    message += ": overlay callback must have signature () -> Unit";
    rt_trap(message.c_str());
}

/// @brief Resolve a Game3D callback in the active tree-walker VM.
/// @param vm VM whose module is searched.
/// @param entry Opaque runtime function value.
/// @param api Runtime API name included in a resolution trap.
/// @return Borrowed function pointer; traps and returns null if unresolved.
static const il::core::Function *resolveStdGame3DCallback(il::vm::VM &vm,
                                                          void *entry,
                                                          const char *api) {
    const il::core::Function *fn = resolveILEntry(vm.module(), entry);
    if (!fn) {
        std::string message(api);
        message += ": callback is not a function in the active VM module";
        rt_trap(message.c_str());
    }
    return fn;
}

/// @brief Resolve a Game3D callback in the active bytecode module.
/// @param module Module whose function table is searched.
/// @param entry Tagged or raw bytecode function value.
/// @param api Runtime API name included in a resolution trap.
/// @return Borrowed function pointer; traps and returns null if unresolved.
static const BytecodeFunction *resolveBytecodeGame3DCallback(const BytecodeModule *module,
                                                             void *entry,
                                                             const char *api) {
    const BytecodeFunction *fn = resolveBytecodeEntry(module, entry);
    if (!fn) {
        std::string message(api);
        message += ": callback is not a function in the active bytecode module";
        rt_trap(message.c_str());
    }
    return fn;
}

/// @brief Native Game3D update trampoline that re-enters the active VM.
/// @param dt Simulation time step forwarded as the callback's sole argument.
/// @details Selects the tree-walker or bytecode callback recorded in
///          `tlsUnifiedGame3DScope` and converts all failures to runtime traps.
extern "C" void unified_game3d_update_trampoline(double dt) {
    UnifiedGame3DCallbackScope *scope = tlsUnifiedGame3DScope;
    if (!scope) {
        rt_trap("Game3D.World3D: invalid VM update callback scope");
        return;
    }

    try {
        if (scope->stdVm && scope->stdUpdate) {
            il::support::SmallVector<il::vm::Slot, 1> args;
            il::vm::Slot dtSlot{};
            dtSlot.f64 = dt;
            args.push_back(dtSlot);
            il::vm::detail::VMAccess::callFunction(*scope->stdVm, *scope->stdUpdate, args);
            return;
        }
        if (scope->bcVm && scope->bcUpdate) {
            std::vector<BCSlot> args;
            BCSlot dtSlot{};
            dtSlot.f64 = dt;
            args.push_back(dtSlot);
            if (!scope->bcVm->invokeVoidReentrant(scope->bcUpdate, args)) {
                const std::string &message = scope->bcVm->trapMessage();
                rt_trap(message.empty() ? "Game3D.World3D: trapped bytecode update callback"
                                        : message.c_str());
            }
            return;
        }
    } catch (const il::vm::RuntimeTrapSignal &signal) {
        rt_trap(signal.message.empty() ? "Game3D.World3D: trapped VM update callback"
                                       : signal.message.c_str());
        return;
    } catch (const std::exception &ex) {
        const std::string message =
            std::string("Game3D.World3D: unhandled VM update exception: ") + ex.what();
        rt_trap(message.c_str());
        return;
    } catch (...) {
        rt_trap("Game3D.World3D: unhandled VM update exception");
        return;
    }

    rt_trap("Game3D.World3D: invalid VM update callback");
}

/// @brief Native Game3D overlay trampoline that re-enters the active VM.
/// @details Invokes the zero-argument callback in `tlsUnifiedGame3DScope` and
///          converts VM traps or C++ exceptions to runtime traps.
extern "C" void unified_game3d_overlay_trampoline(void) {
    UnifiedGame3DCallbackScope *scope = tlsUnifiedGame3DScope;
    if (!scope) {
        rt_trap("Game3D.World3D: invalid VM overlay callback scope");
        return;
    }

    try {
        if (scope->stdVm && scope->stdOverlay) {
            il::support::SmallVector<il::vm::Slot, 1> args;
            il::vm::detail::VMAccess::callFunction(*scope->stdVm, *scope->stdOverlay, args);
            return;
        }
        if (scope->bcVm && scope->bcOverlay) {
            std::vector<BCSlot> args;
            if (!scope->bcVm->invokeVoidReentrant(scope->bcOverlay, args)) {
                const std::string &message = scope->bcVm->trapMessage();
                rt_trap(message.empty() ? "Game3D.World3D: trapped bytecode overlay callback"
                                        : message.c_str());
            }
            return;
        }
    } catch (const il::vm::RuntimeTrapSignal &signal) {
        rt_trap(signal.message.empty() ? "Game3D.World3D: trapped VM overlay callback"
                                       : signal.message.c_str());
        return;
    } catch (const std::exception &ex) {
        const std::string message =
            std::string("Game3D.World3D: unhandled VM overlay exception: ") + ex.what();
        rt_trap(message.c_str());
        return;
    } catch (...) {
        rt_trap("Game3D.World3D: unhandled VM overlay exception");
        return;
    }

    rt_trap("Game3D.World3D: invalid VM overlay callback");
}

template <typename Fn>
/// @brief Run a native Game3D loop with VM callback context and trap recovery.
/// @tparam Fn Nullary callable that enters the native game loop.
/// @param scope Callback dispatch context to publish thread-locally.
/// @param fn Native loop invocation.
/// @details Restores the previous callback context and runtime recovery target
///          before propagating any captured native trap.
static void invokeUnifiedGame3DLoop(UnifiedGame3DCallbackScope &scope, Fn &&fn) {
    char trapMessage[512] = "";
    int trapped = 0;
    jmp_buf recovery;

    scope.previous = tlsUnifiedGame3DScope;
    tlsUnifiedGame3DScope = &scope;
    rt_trap_set_recovery(&recovery);
    RT_SUPPRESS_SETJMP_WARNING_BEGIN;
    const int recoveryState = setjmp(recovery);
    RT_SUPPRESS_SETJMP_WARNING_END;
    if (recoveryState != 0) {
        const char *msg = rt_trap_get_error();
        std::snprintf(trapMessage, sizeof(trapMessage), "%s", msg && msg[0] ? msg : "Game3D trap");
        trapped = 1;
    } else {
        fn();
    }
    rt_trap_clear_recovery();
    tlsUnifiedGame3DScope = scope.previous;
    if (trapped)
        rt_trap(trapMessage);
}

/// @brief Bridge `World3D.run` callbacks for either interpreter backend.
/// @param args Runtime ABI slots containing `(world, update)`.
/// @param result Unused void-result storage.
/// @details Re-enters the active VM when the update is interpreted, otherwise
///          chains to the previously registered or native handler.
static void unified_game3d_run_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *update = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;

    if (il::vm::VM *stdVm = il::vm::activeVMInstance()) {
        const il::core::Function *updateFn =
            resolveStdGame3DCallback(*stdVm, update, "Game3D.World3D.run");
        validateGame3DUpdateSignature(*updateFn, "Game3D.World3D.run");
        UnifiedGame3DCallbackScope scope{
            stdVm, nullptr, updateFn, nullptr, nullptr, nullptr, nullptr};
        /// @brief Run the standard VM update callback through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run(world, reinterpret_cast<void *>(&unified_game3d_update_trampoline));
        });
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *updateFn =
            resolveBytecodeGame3DCallback(bcModule, update, "Game3D.World3D.run");
        validateBytecodeGame3DUpdateSignature(*updateFn, "Game3D.World3D.run");
        UnifiedGame3DCallbackScope scope{
            nullptr, bcVm, nullptr, nullptr, updateFn, nullptr, nullptr};
        /// @brief Run the bytecode VM update callback through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run(world, reinterpret_cast<void *>(&unified_game3d_update_trampoline));
        });
        return;
    }

    if (gPriorGame3DRunHandler) {
        gPriorGame3DRunHandler(args, result);
        return;
    }
    rt_game3d_world_run(world, update);
}

/// @brief Bridge `World3D.runWithOverlay` update and overlay callbacks.
/// @param args Runtime ABI slots containing `(world, update, overlay)`.
/// @param result Unused void-result storage.
static void unified_game3d_run_with_overlay_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *update = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *overlay = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;

    if (il::vm::VM *stdVm = il::vm::activeVMInstance()) {
        const il::core::Function *updateFn =
            resolveStdGame3DCallback(*stdVm, update, "Game3D.World3D.runWithOverlay");
        const il::core::Function *overlayFn =
            resolveStdGame3DCallback(*stdVm, overlay, "Game3D.World3D.runWithOverlay");
        validateGame3DUpdateSignature(*updateFn, "Game3D.World3D.runWithOverlay");
        validateGame3DOverlaySignature(*overlayFn, "Game3D.World3D.runWithOverlay");
        UnifiedGame3DCallbackScope scope{
            stdVm, nullptr, updateFn, overlayFn, nullptr, nullptr, nullptr};
        /// @brief Run standard VM update and overlay callbacks through unified trampolines.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_with_overlay(
                world,
                reinterpret_cast<void *>(&unified_game3d_update_trampoline),
                reinterpret_cast<void *>(&unified_game3d_overlay_trampoline));
        });
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *updateFn =
            resolveBytecodeGame3DCallback(bcModule, update, "Game3D.World3D.runWithOverlay");
        const BytecodeFunction *overlayFn =
            resolveBytecodeGame3DCallback(bcModule, overlay, "Game3D.World3D.runWithOverlay");
        validateBytecodeGame3DUpdateSignature(*updateFn, "Game3D.World3D.runWithOverlay");
        validateBytecodeGame3DOverlaySignature(*overlayFn, "Game3D.World3D.runWithOverlay");
        UnifiedGame3DCallbackScope scope{
            nullptr, bcVm, nullptr, nullptr, updateFn, overlayFn, nullptr};
        /// @brief Run bytecode VM update and overlay callbacks through unified trampolines.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_with_overlay(
                world,
                reinterpret_cast<void *>(&unified_game3d_update_trampoline),
                reinterpret_cast<void *>(&unified_game3d_overlay_trampoline));
        });
        return;
    }

    if (gPriorGame3DRunWithOverlayHandler) {
        gPriorGame3DRunWithOverlayHandler(args, result);
        return;
    }
    rt_game3d_world_run_with_overlay(world, update, overlay);
}

/// @brief Bridge `World3D.runFixed` callbacks for either interpreter backend.
/// @param args Runtime ABI slots containing `(world, step, update)`.
/// @param result Unused void-result storage.
static void unified_game3d_run_fixed_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    double step = args && args[1] ? *reinterpret_cast<double *>(args[1]) : 0.0;
    void *update = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;

    if (il::vm::VM *stdVm = il::vm::activeVMInstance()) {
        const il::core::Function *updateFn =
            resolveStdGame3DCallback(*stdVm, update, "Game3D.World3D.runFixed");
        validateGame3DUpdateSignature(*updateFn, "Game3D.World3D.runFixed");
        UnifiedGame3DCallbackScope scope{
            stdVm, nullptr, updateFn, nullptr, nullptr, nullptr, nullptr};
        /// @brief Run the standard VM fixed-step callback through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_fixed(
                world, step, reinterpret_cast<void *>(&unified_game3d_update_trampoline));
        });
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *updateFn =
            resolveBytecodeGame3DCallback(bcModule, update, "Game3D.World3D.runFixed");
        validateBytecodeGame3DUpdateSignature(*updateFn, "Game3D.World3D.runFixed");
        UnifiedGame3DCallbackScope scope{
            nullptr, bcVm, nullptr, nullptr, updateFn, nullptr, nullptr};
        /// @brief Run the bytecode VM fixed-step callback through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_fixed(
                world, step, reinterpret_cast<void *>(&unified_game3d_update_trampoline));
        });
        return;
    }

    if (gPriorGame3DRunFixedHandler) {
        gPriorGame3DRunFixedHandler(args, result);
        return;
    }
    rt_game3d_world_run_fixed(world, step, update);
}

/// @brief Bridge `World3D.runFixedWithOverlay` callbacks for either interpreter.
/// @param args Runtime ABI slots containing `(world, step, update, overlay)`.
/// @param result Unused void-result storage.
static void unified_game3d_run_fixed_with_overlay_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    double step = args && args[1] ? *reinterpret_cast<double *>(args[1]) : 0.0;
    void *update = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *overlay = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;

    if (il::vm::VM *stdVm = il::vm::activeVMInstance()) {
        const il::core::Function *updateFn =
            resolveStdGame3DCallback(*stdVm, update, "Game3D.World3D.runFixedWithOverlay");
        const il::core::Function *overlayFn =
            resolveStdGame3DCallback(*stdVm, overlay, "Game3D.World3D.runFixedWithOverlay");
        validateGame3DUpdateSignature(*updateFn, "Game3D.World3D.runFixedWithOverlay");
        validateGame3DOverlaySignature(*overlayFn, "Game3D.World3D.runFixedWithOverlay");
        UnifiedGame3DCallbackScope scope{
            stdVm, nullptr, updateFn, overlayFn, nullptr, nullptr, nullptr};
        /// @brief Run standard VM fixed-step callbacks through unified trampolines.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_fixed_with_overlay(
                world,
                step,
                reinterpret_cast<void *>(&unified_game3d_update_trampoline),
                reinterpret_cast<void *>(&unified_game3d_overlay_trampoline));
        });
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *updateFn =
            resolveBytecodeGame3DCallback(bcModule, update, "Game3D.World3D.runFixedWithOverlay");
        const BytecodeFunction *overlayFn =
            resolveBytecodeGame3DCallback(bcModule, overlay, "Game3D.World3D.runFixedWithOverlay");
        validateBytecodeGame3DUpdateSignature(*updateFn, "Game3D.World3D.runFixedWithOverlay");
        validateBytecodeGame3DOverlaySignature(*overlayFn, "Game3D.World3D.runFixedWithOverlay");
        UnifiedGame3DCallbackScope scope{
            nullptr, bcVm, nullptr, nullptr, updateFn, overlayFn, nullptr};
        /// @brief Run bytecode VM fixed-step callbacks through unified trampolines.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_fixed_with_overlay(
                world,
                step,
                reinterpret_cast<void *>(&unified_game3d_update_trampoline),
                reinterpret_cast<void *>(&unified_game3d_overlay_trampoline));
        });
        return;
    }

    if (gPriorGame3DRunFixedWithOverlayHandler) {
        gPriorGame3DRunFixedWithOverlayHandler(args, result);
        return;
    }
    rt_game3d_world_run_fixed_with_overlay(world, step, update, overlay);
}

/// @brief Bridge bounded `World3D.runFrames` callbacks for either interpreter.
/// @param args Runtime ABI slots containing `(world, frames, step, update)`.
/// @param result Unused void-result storage.
static void unified_game3d_run_frames_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    int64_t frames = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
    double step = args && args[2] ? *reinterpret_cast<double *>(args[2]) : 0.0;
    void *update = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;

    if (il::vm::VM *stdVm = il::vm::activeVMInstance()) {
        const il::core::Function *updateFn =
            resolveStdGame3DCallback(*stdVm, update, "Game3D.World3D.runFrames");
        validateGame3DUpdateSignature(*updateFn, "Game3D.World3D.runFrames");
        UnifiedGame3DCallbackScope scope{
            stdVm, nullptr, updateFn, nullptr, nullptr, nullptr, nullptr};
        /// @brief Run the standard VM bounded-frame callback through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_frames(
                world, frames, step, reinterpret_cast<void *>(&unified_game3d_update_trampoline));
        });
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *updateFn =
            resolveBytecodeGame3DCallback(bcModule, update, "Game3D.World3D.runFrames");
        validateBytecodeGame3DUpdateSignature(*updateFn, "Game3D.World3D.runFrames");
        UnifiedGame3DCallbackScope scope{
            nullptr, bcVm, nullptr, nullptr, updateFn, nullptr, nullptr};
        /// @brief Run the bytecode VM bounded-frame callback through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_run_frames(
                world, frames, step, reinterpret_cast<void *>(&unified_game3d_update_trampoline));
        });
        return;
    }

    if (gPriorGame3DRunFramesHandler) {
        gPriorGame3DRunFramesHandler(args, result);
        return;
    }
    rt_game3d_world_run_frames(world, frames, step, update);
}

/// @brief Bridge one-shot `World3D.drawOverlay` callbacks for either interpreter.
/// @param args Runtime ABI slots containing `(world, overlay)`.
/// @param result Unused void-result storage.
static void unified_game3d_draw_overlay_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *overlay = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;

    if (il::vm::VM *stdVm = il::vm::activeVMInstance()) {
        const il::core::Function *overlayFn =
            resolveStdGame3DCallback(*stdVm, overlay, "Game3D.World3D.drawOverlay");
        validateGame3DOverlaySignature(*overlayFn, "Game3D.World3D.drawOverlay");
        UnifiedGame3DCallbackScope scope{
            stdVm, nullptr, nullptr, overlayFn, nullptr, nullptr, nullptr};
        /// @brief Draw one standard VM overlay through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_draw_overlay(
                world, reinterpret_cast<void *>(&unified_game3d_overlay_trampoline));
        });
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *overlayFn =
            resolveBytecodeGame3DCallback(bcModule, overlay, "Game3D.World3D.drawOverlay");
        validateBytecodeGame3DOverlaySignature(*overlayFn, "Game3D.World3D.drawOverlay");
        UnifiedGame3DCallbackScope scope{
            nullptr, bcVm, nullptr, nullptr, nullptr, overlayFn, nullptr};
        /// @brief Draw one bytecode VM overlay through the unified trampoline.
        invokeUnifiedGame3DLoop(scope, [&]() {
            rt_game3d_world_draw_overlay(
                world, reinterpret_cast<void *>(&unified_game3d_overlay_trampoline));
        });
        return;
    }

    if (gPriorGame3DDrawOverlayHandler) {
        gPriorGame3DDrawOverlayHandler(args, result);
        return;
    }
    rt_game3d_world_draw_overlay(world, overlay);
}

/// @brief Handle `Zanna.Threads.Thread.Start` for either interpreter backend.
/// @param args Runtime ABI slots containing `(entry, argument)`.
/// @param result Optional slot receiving the native thread handle.
/// @details Resolves and validates an interpreted entry, snapshots the backing
///          program/module state into a worker payload, and starts the matching
///          native trampoline. Calls outside an interpreter chain to the prior
///          handler or native runtime.
static void unified_thread_start_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry) {
        rt_trap("Thread.Start: null entry");
        return;
    }

    // Check for standard VM first
    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("Thread.Start: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("Thread.Start: invalid entry");
            return;
        }
        validateEntrySignature(*entryFn);

        auto *payload = new (std::nothrow) VmThreadStartPayload{
            &module, std::move(program), stdVm->externRegistry(), entryFn, arg, false};
        if (!payload) {
            rt_trap("Thread.Start: payload allocation failed");
            return;
        }
        il::vm::retainExternRegistry(payload->externRegistry);
        void *thread =
            rt_thread_start(reinterpret_cast<void *>(&vm_thread_entry_trampoline_bc), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
            delete payload;
            rt_trap("Thread.Start: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // Check for BytecodeVM
    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("Thread.Start: invalid bytecode entry");
            return;
        }
        validateBytecodeThreadEntrySignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("Thread.Start: bytecode module snapshot failed");
            return;
        }
        auto *payload =
            new (std::nothrow) BytecodeThreadPayload{moduleOwner,
                                                     moduleOwner.get(),
                                                     ownedEntryFn,
                                                     arg,
                                                     false,
                                                     bcVm->captureExecutionEnvironment()};
        if (!payload) {
            rt_trap("Thread.Start: payload allocation failed");
            return;
        }
        void *thread =
            rt_thread_start(reinterpret_cast<void *>(&bytecode_thread_entry_trampoline), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            delete payload;
            rt_trap("Thread.Start: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // No VM active - direct call (native code path)
    if (gPriorThreadStartHandler) {
        gPriorThreadStartHandler(args, result);
        return;
    }
    void *thread = rt_thread_start(entry, arg);
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Handle `Thread.StartOwned`, transferring the argument to the worker.
/// @param args Runtime ABI slots containing `(entry, owned argument)`.
/// @param result Optional slot receiving the native thread handle.
/// @details Mirrors @ref unified_thread_start_handler while ensuring every
///          setup-failure and worker-completion path releases the owned object.
static void unified_thread_start_owned_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry) {
        rt_trap("Thread.StartOwned: null entry");
        return;
    }

    // Check for standard VM first
    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("Thread.StartOwned: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("Thread.StartOwned: invalid entry");
            return;
        }
        validateEntrySignature(*entryFn);

        auto *payload = new (std::nothrow) VmThreadStartPayload{
            &module, std::move(program), stdVm->externRegistry(), entryFn, arg, arg != nullptr};
        if (!payload) {
            rt_trap("Thread.StartOwned: payload allocation failed");
            return;
        }
        if (payload->ownsArg)
            rt_obj_retain_maybe(arg);
        il::vm::retainExternRegistry(payload->externRegistry);
        void *thread =
            rt_thread_start(reinterpret_cast<void *>(&vm_thread_entry_trampoline_bc), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
            delete payload;
            rt_trap("Thread.StartOwned: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // Check for BytecodeVM
    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("Thread.StartOwned: invalid bytecode entry");
            return;
        }
        validateBytecodeThreadEntrySignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("Thread.StartOwned: bytecode module snapshot failed");
            return;
        }
        auto *payload =
            new (std::nothrow) BytecodeThreadPayload{moduleOwner,
                                                     moduleOwner.get(),
                                                     ownedEntryFn,
                                                     arg,
                                                     arg != nullptr,
                                                     bcVm->captureExecutionEnvironment()};
        if (!payload) {
            rt_trap("Thread.StartOwned: payload allocation failed");
            return;
        }
        if (payload->ownsArg)
            rt_obj_retain_maybe(arg);
        void *thread =
            rt_thread_start(reinterpret_cast<void *>(&bytecode_thread_entry_trampoline), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            delete payload;
            rt_trap("Thread.StartOwned: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // No VM active - direct call (native code path)
    if (gPriorThreadStartOwnedHandler) {
        gPriorThreadStartOwnedHandler(args, result);
        return;
    }
    void *thread = rt_thread_start_owned(entry, arg);
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Handle trap-recoverable `Thread.StartSafe` for either interpreter.
/// @param args Runtime ABI slots containing `(entry, argument)`.
/// @param result Optional slot receiving the safe-thread handle.
/// @details Uses `rt_thread_start_safe` and the safe trampolines so native
///          `setjmp`/`longjmp` recovery can capture interpreted traps.
static void unified_thread_start_safe_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry) {
        rt_trap("Thread.StartSafe: null entry");
        return;
    }

    // Check for standard VM first
    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("Thread.StartSafe: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("Thread.StartSafe: invalid entry");
            return;
        }
        validateEntrySignature(*entryFn);

        auto *payload = new (std::nothrow) VmThreadStartPayload{
            &module, std::move(program), stdVm->externRegistry(), entryFn, arg, false};
        if (!payload) {
            rt_trap("Thread.StartSafe: payload allocation failed");
            return;
        }
        il::vm::retainExternRegistry(payload->externRegistry);
        void *thread = rt_thread_start_safe(
            reinterpret_cast<void *>(&vm_thread_safe_entry_trampoline_bc), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
            delete payload;
            rt_trap("Thread.StartSafe: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // Check for BytecodeVM
    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("Thread.StartSafe: invalid bytecode entry");
            return;
        }
        validateBytecodeThreadEntrySignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("Thread.StartSafe: bytecode module snapshot failed");
            return;
        }
        auto *payload =
            new (std::nothrow) BytecodeThreadPayload{moduleOwner,
                                                     moduleOwner.get(),
                                                     ownedEntryFn,
                                                     arg,
                                                     false,
                                                     bcVm->captureExecutionEnvironment()};
        if (!payload) {
            rt_trap("Thread.StartSafe: payload allocation failed");
            return;
        }
        void *thread = rt_thread_start_safe(
            reinterpret_cast<void *>(&bytecode_thread_safe_entry_trampoline), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            delete payload;
            rt_trap("Thread.StartSafe: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // No VM active - direct call (native code path)
    if (gPriorThreadStartSafeHandler) {
        gPriorThreadStartSafeHandler(args, result);
        return;
    }
    void *thread = rt_thread_start_safe(entry, arg);
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Handle `Thread.StartSafeOwned` with trap recovery and argument transfer.
/// @param args Runtime ABI slots containing `(entry, owned argument)`.
/// @param result Optional slot receiving the safe-thread handle.
/// @details Combines the safe-thread recovery contract with deterministic
///          release of the owned argument on all setup and completion paths.
static void unified_thread_start_safe_owned_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry) {
        rt_trap("Thread.StartSafeOwned: null entry");
        return;
    }

    // Check for standard VM first
    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("Thread.StartSafeOwned: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("Thread.StartSafeOwned: invalid entry");
            return;
        }
        validateEntrySignature(*entryFn);

        auto *payload = new (std::nothrow) VmThreadStartPayload{
            &module, std::move(program), stdVm->externRegistry(), entryFn, arg, arg != nullptr};
        if (!payload) {
            rt_trap("Thread.StartSafeOwned: payload allocation failed");
            return;
        }
        if (payload->ownsArg)
            rt_obj_retain_maybe(arg);
        il::vm::retainExternRegistry(payload->externRegistry);
        void *thread = rt_thread_start_safe(
            reinterpret_cast<void *>(&vm_thread_safe_entry_trampoline_bc), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
            delete payload;
            rt_trap("Thread.StartSafeOwned: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // Check for BytecodeVM
    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("Thread.StartSafeOwned: invalid bytecode entry");
            return;
        }
        validateBytecodeThreadEntrySignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("Thread.StartSafeOwned: bytecode module snapshot failed");
            return;
        }
        auto *payload =
            new (std::nothrow) BytecodeThreadPayload{moduleOwner,
                                                     moduleOwner.get(),
                                                     ownedEntryFn,
                                                     arg,
                                                     arg != nullptr,
                                                     bcVm->captureExecutionEnvironment()};
        if (!payload) {
            rt_trap("Thread.StartSafeOwned: payload allocation failed");
            return;
        }
        if (payload->ownsArg)
            rt_obj_retain_maybe(arg);
        void *thread = rt_thread_start_safe(
            reinterpret_cast<void *>(&bytecode_thread_safe_entry_trampoline), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            delete payload;
            rt_trap("Thread.StartSafeOwned: failed to create thread");
            return;
        }
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    // No VM active - direct call (native code path)
    if (gPriorThreadStartSafeOwnedHandler) {
        gPriorThreadStartSafeOwnedHandler(args, result);
        return;
    }
    void *thread = rt_thread_start_safe_owned(entry, arg);
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief C ABI trampoline: run a standard-VM Async.Run entry from a bytecode
///        context. Decodes the payload, executes the entry, settles the
///        promise with the result or error, and frees the payload + owned arg.
/// @param raw Owned `VmAsyncRunPayload`.
extern "C" void vm_async_run_entry_trampoline_bc(void *raw) {
    VmAsyncRunPayload *payload = static_cast<VmAsyncRunPayload *>(raw);
    if (!payload || !payload->module || !payload->entry || !payload->promise) {
        if (payload) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
        }
        delete payload;
        rt_abort("Async.Run: invalid entry");
        return;
    }

    il::vm::Slot result{};
    bool completed = false;
    rt_string error = nullptr;

    try {
        {
            il::vm::VM vm(*payload->module, payload->program);
            vm.setExternRegistry(payload->externRegistry);
            il::support::SmallVector<il::vm::Slot, 2> args;
            il::vm::Slot s{};
            s.ptr = payload->arg;
            args.push_back(s);
            result = il::vm::detail::VMAccess::callFunction(vm, *payload->entry, args);
        }
        completed = true;
    } catch (const il::vm::RuntimeTrapSignal &signal) {
        const char *message =
            signal.message.empty() ? "Async.Run: trapped VM worker" : signal.message.c_str();
        error = rt_string_from_bytes(message, std::strlen(message));
    } catch (const std::exception &ex) {
        std::string message = std::string("Async.Run: unhandled exception: ") + ex.what();
        error = rt_string_from_bytes(message.data(), message.size());
    } catch (...) {
        error = rt_const_cstr("Async.Run: unhandled non-standard exception");
    }

    if (completed) {
        completeAsyncPromiseWithResult(
            payload->promise, result.ptr, &payload->arg, &payload->ownsArg);
    } else {
        completeAsyncPromiseWithError(payload->promise, error, &payload->arg, &payload->ownsArg);
        rt_str_release_maybe(error);
    }

    if (rt_obj_release_check0(payload->promise))
        rt_obj_free(payload->promise);
    il::vm::releaseExternRegistry(payload->externRegistry);
    delete payload;
}

/// @brief C ABI trampoline: run a bytecode Async.Run entry on a worker VM,
///        settling its promise with the returned result or an error and
///        releasing the payload.
/// @param raw Owned `BytecodeAsyncPayload`.
extern "C" void bytecode_async_entry_trampoline(void *raw) {
    BytecodeAsyncPayload *payload = static_cast<BytecodeAsyncPayload *>(raw);
    if (payload && payload->moduleOwner)
        payload->module = payload->moduleOwner.get();
    if (!payload || !payload->module || !payload->entry || !payload->promise) {
        if (payload)
            releaseWorkerArg(payload->arg, payload->ownsArg);
        delete payload;
        rt_abort("Async.Run: invalid bytecode entry");
        return;
    }

    BCSlot result{};
    VMState workerState = VMState::Ready;
    std::string trapMessage;

    {
        BytecodeVM vm;
        vm.load(payload->module);
        vm.applyExecutionEnvironment(payload->environment);

        std::vector<BCSlot> args;
        BCSlot argSlot{};
        argSlot.ptr = payload->arg;
        args.push_back(argSlot);

        result = vm.exec(payload->entry, args);
        workerState = vm.state();
        if (workerState == VMState::Trapped)
            trapMessage = vm.trapMessage();
    }

    if (workerState == VMState::Trapped) {
        rt_string error = trapMessage.empty()
                              ? rt_const_cstr("Async.Run: trapped")
                              : rt_string_from_bytes(trapMessage.data(), trapMessage.size());
        completeAsyncPromiseWithError(payload->promise, error, &payload->arg, &payload->ownsArg);
        rt_str_release_maybe(error);
    } else {
        completeAsyncPromiseWithResult(
            payload->promise, result.ptr, &payload->arg, &payload->ownsArg);
    }

    if (rt_obj_release_check0(payload->promise))
        rt_obj_free(payload->promise);
    delete payload;
}

/// @brief C ABI trampoline: dispatch one HTTP request to a standard-VM
///        bind-handler from a bytecode context (@p req/@p res are the
///        request/response objects).
/// @param raw Borrowed persistent `VmHttpHandlerPayload`.
/// @param req Borrowed runtime request object.
/// @param res Borrowed runtime response object.
extern "C" void vm_http_handler_dispatch_bc(void *raw, void *req, void *res) {
    auto *payload = static_cast<VmHttpHandlerPayload *>(raw);
    if (!payload || !payload->module || !payload->entry) {
        rt_abort("HttpServer.BindHandler: invalid entry");
        return;
    }

    try {
        il::vm::VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);
        il::support::SmallVector<il::vm::Slot, 2> args;
        il::vm::Slot reqSlot{};
        reqSlot.ptr = req;
        args.push_back(reqSlot);
        il::vm::Slot resSlot{};
        resSlot.ptr = res;
        args.push_back(resSlot);
        il::vm::detail::VMAccess::callFunction(vm, *payload->entry, args);
    } catch (...) {
        rt_abort("HttpServer.BindHandler: unhandled exception");
    }
}

/// @brief C ABI trampoline: dispatch one HTTP request to a bytecode
///        bind-handler, invoking its entry on the active VM with the
///        request/response pair.
/// @param raw Borrowed persistent `BytecodeHttpHandlerPayload`.
/// @param req Borrowed runtime request object.
/// @param res Borrowed runtime response object.
extern "C" void bytecode_http_handler_dispatch(void *raw, void *req, void *res) {
    auto *payload = static_cast<BytecodeHttpHandlerPayload *>(raw);
    if (payload && payload->moduleOwner)
        payload->module = payload->moduleOwner.get();
    if (!payload || !payload->module || !payload->entry) {
        rt_abort("HttpServer.BindHandler: invalid bytecode entry");
        return;
    }

    BytecodeVM vm;
    vm.load(payload->module);
    vm.applyExecutionEnvironment(payload->environment);

    std::vector<BCSlot> args;
    BCSlot reqSlot{};
    reqSlot.ptr = req;
    args.push_back(reqSlot);
    BCSlot resSlot{};
    resSlot.ptr = res;
    args.push_back(resSlot);
    vm.exec(payload->entry, args);
    if (vm.state() == VMState::Trapped) {
        const std::string message = vm.trapMessage();
        rt_abort(message.empty() ? "HttpServer.BindHandler: trapped bytecode handler"
                                 : message.c_str());
    }
}

/// @brief C ABI destructor for a standard-VM HTTP handler payload (called by
///        the runtime when the bound handler is torn down).
/// @param raw Owned `VmHttpHandlerPayload`, or null.
extern "C" void destroy_vm_http_handler_payload_bc(void *raw) {
    auto *payload = static_cast<VmHttpHandlerPayload *>(raw);
    if (!payload)
        return;
    il::vm::releaseExternRegistry(payload->externRegistry);
    delete payload;
}

/// @brief C ABI destructor for a bytecode HTTP handler payload.
/// @param raw Owned `BytecodeHttpHandlerPayload`.
extern "C" void destroy_bytecode_http_handler_payload(void *raw) {
    delete static_cast<BytecodeHttpHandlerPayload *>(raw);
}

/// @brief Runtime handler for HttpServer.BindHandler that works for both the
///        standard VM and the bytecode VM: detects which engine the entry
///        belongs to, validates its signature, and registers the matching
///        dispatch + payload-destructor trampolines.
/// @param args Runtime ABI slots containing `(server, route tag, entry)`.
/// @param result Optional result slot forwarded to prior/native handlers.
static void unified_http_server_bind_handler(void **args, void *result) {
    (void)result;

    void *server = nullptr;
    rt_string tag = nullptr;
    void *entry = nullptr;
    if (args && args[0])
        server = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        tag = *reinterpret_cast<rt_string *>(args[1]);
    if (args && args[2])
        entry = *reinterpret_cast<void **>(args[2]);

    if (!entry) {
        rt_trap("HttpServer.BindHandler: null entry");
        return;
    }

    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("HttpServer.BindHandler: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("HttpServer.BindHandler: invalid entry");
            return;
        }
        validateHttpHandlerSignature(*entryFn);

        auto *payload =
            new VmHttpHandlerPayload{&module, std::move(program), stdVm->externRegistry(), entryFn};
        il::vm::retainExternRegistry(payload->externRegistry);
        rt_http_server_bind_handler_dispatch(
            server,
            tag,
            reinterpret_cast<void *>(&vm_http_handler_dispatch_bc),
            payload,
            reinterpret_cast<void *>(&destroy_vm_http_handler_payload_bc));
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("HttpServer.BindHandler: invalid bytecode entry");
            return;
        }
        validateBytecodeHttpHandlerSignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("HttpServer.BindHandler: bytecode module snapshot failed");
            return;
        }
        auto *payload = new BytecodeHttpHandlerPayload{
            moduleOwner, moduleOwner.get(), ownedEntryFn, bcVm->captureExecutionEnvironment()};
        rt_http_server_bind_handler_dispatch(
            server,
            tag,
            reinterpret_cast<void *>(&bytecode_http_handler_dispatch),
            payload,
            reinterpret_cast<void *>(&destroy_bytecode_http_handler_payload));
        return;
    }

    if (gPriorHttpBindHandler) {
        gPriorHttpBindHandler(args, result);
        return;
    }
    rt_http_server_bind_handler(server, tag, entry);
}

/// @brief HTTPS counterpart of @ref unified_http_server_bind_handler (same
///        dual-engine dispatch over a TLS server).
/// @param args Runtime ABI slots containing `(server, route tag, entry)`.
/// @param result Optional result slot forwarded to prior/native handlers.
static void unified_https_server_bind_handler(void **args, void *result) {
    (void)result;

    void *server = nullptr;
    rt_string tag = nullptr;
    void *entry = nullptr;
    if (args && args[0])
        server = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        tag = *reinterpret_cast<rt_string *>(args[1]);
    if (args && args[2])
        entry = *reinterpret_cast<void **>(args[2]);

    if (!entry) {
        rt_trap("HttpsServer.BindHandler: null entry");
        return;
    }

    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("HttpsServer.BindHandler: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("HttpsServer.BindHandler: invalid entry");
            return;
        }
        validateHttpsHandlerSignature(*entryFn);

        auto *payload =
            new VmHttpHandlerPayload{&module, std::move(program), stdVm->externRegistry(), entryFn};
        il::vm::retainExternRegistry(payload->externRegistry);
        rt_https_server_bind_handler_dispatch(
            server,
            tag,
            reinterpret_cast<void *>(&vm_http_handler_dispatch_bc),
            payload,
            reinterpret_cast<void *>(&destroy_vm_http_handler_payload_bc));
        return;
    }

    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("HttpsServer.BindHandler: invalid bytecode entry");
            return;
        }
        validateBytecodeHttpsHandlerSignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("HttpsServer.BindHandler: bytecode module snapshot failed");
            return;
        }
        auto *payload = new BytecodeHttpHandlerPayload{
            moduleOwner, moduleOwner.get(), ownedEntryFn, bcVm->captureExecutionEnvironment()};
        rt_https_server_bind_handler_dispatch(
            server,
            tag,
            reinterpret_cast<void *>(&bytecode_http_handler_dispatch),
            payload,
            reinterpret_cast<void *>(&destroy_bytecode_http_handler_payload));
        return;
    }

    if (gPriorHttpsBindHandler) {
        gPriorHttpsBindHandler(args, result);
        return;
    }
    rt_https_server_bind_handler(server, tag, entry);
}

/// @brief Runtime handler for Async.Run usable from both engines: validates
///        the entry signature, builds the appropriate async payload, and
///        schedules it on a worker, returning the promise.
/// @param args Runtime ABI slots containing `(entry, argument)`.
/// @param result Optional slot receiving the future handle.
/// @param owned Whether the worker takes an owned reference to the argument.
static void unified_async_run_impl(void **args, void *result, bool owned) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry) {
        rt_trap("Async.Run: null entry");
        return;
    }

    // Standard VM path
    il::vm::VM *stdVm = il::vm::activeVMInstance();
    if (stdVm) {
        std::shared_ptr<il::vm::VM::ProgramState> program = stdVm->programState();
        if (!program) {
            rt_trap("Async.Run: invalid runtime state");
            return;
        }

        const il::core::Module &module = stdVm->module();
        const il::core::Function *entryFn = resolveILEntry(module, entry);
        if (!entryFn) {
            rt_trap("Async.Run: invalid entry");
            return;
        }
        validateAsyncEntrySignature(*entryFn);

        void *promise = rt_promise_new();
        if (!promise) {
            rt_trap("Async.Run: promise allocation failed");
            return;
        }
        void *future = rt_promise_get_future(promise);
        if (!future) {
            if (rt_obj_release_check0(promise))
                rt_obj_free(promise);
            rt_trap("Async.Run: future allocation failed");
            return;
        }
        auto *payload = new (std::nothrow) VmAsyncRunPayload{&module,
                                                             std::move(program),
                                                             stdVm->externRegistry(),
                                                             entryFn,
                                                             arg,
                                                             owned && arg != nullptr,
                                                             promise};
        if (!payload) {
            rt_promise_set_error(promise, rt_const_cstr("Async.Run: payload allocation failed"));
            if (rt_obj_release_check0(promise))
                rt_obj_free(promise);
            publishAsyncFutureResult(result, future);
            return;
        }
        if (payload->ownsArg)
            rt_obj_retain_maybe(arg);
        il::vm::retainExternRegistry(payload->externRegistry);
        void *thread =
            rt_thread_start(reinterpret_cast<void *>(&vm_async_run_entry_trampoline_bc), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            il::vm::releaseExternRegistry(payload->externRegistry);
            delete payload;
            rt_promise_set_error(promise, rt_const_cstr("Async.Run: failed to create thread"));
            if (rt_obj_release_check0(promise))
                rt_obj_free(promise);
            publishAsyncFutureResult(result, future);
            return;
        }

        if (rt_obj_release_check0(thread))
            rt_obj_free(thread);
        publishAsyncFutureResult(result, future);
        return;
    }

    // Bytecode VM path
    BytecodeVM *bcVm = activeBytecodeVMInstance();
    const BytecodeModule *bcModule = activeBytecodeModule();
    if (bcVm && bcModule) {
        const BytecodeFunction *entryFn = resolveBytecodeEntry(bcModule, entry);
        if (!entryFn) {
            rt_trap("Async.Run: invalid bytecode entry");
            return;
        }
        validateBytecodeAsyncEntrySignature(*entryFn);

        auto moduleOwner = cloneBytecodeModuleForWorker(bcModule);
        const BytecodeFunction *ownedEntryFn = resolveClonedBytecodeEntry(moduleOwner, entryFn);
        if (!ownedEntryFn) {
            rt_trap("Async.Run: bytecode module snapshot failed");
            return;
        }
        void *promise = rt_promise_new();
        if (!promise) {
            rt_trap("Async.Run: promise allocation failed");
            return;
        }
        void *future = rt_promise_get_future(promise);
        if (!future) {
            if (rt_obj_release_check0(promise))
                rt_obj_free(promise);
            rt_trap("Async.Run: future allocation failed");
            return;
        }
        auto *payload = new (std::nothrow) BytecodeAsyncPayload{moduleOwner,
                                                                moduleOwner.get(),
                                                                ownedEntryFn,
                                                                arg,
                                                                owned && arg != nullptr,
                                                                bcVm->captureExecutionEnvironment(),
                                                                promise};
        if (!payload) {
            rt_promise_set_error(promise, rt_const_cstr("Async.Run: payload allocation failed"));
            if (rt_obj_release_check0(promise))
                rt_obj_free(promise);
            publishAsyncFutureResult(result, future);
            return;
        }
        if (payload->ownsArg)
            rt_obj_retain_maybe(arg);
        void *thread =
            rt_thread_start(reinterpret_cast<void *>(&bytecode_async_entry_trampoline), payload);
        if (!thread) {
            releaseWorkerArg(payload->arg, payload->ownsArg);
            delete payload;
            rt_promise_set_error(promise, rt_const_cstr("Async.Run: failed to create thread"));
            if (rt_obj_release_check0(promise))
                rt_obj_free(promise);
            publishAsyncFutureResult(result, future);
            return;
        }

        if (rt_obj_release_check0(thread))
            rt_obj_free(thread);
        publishAsyncFutureResult(result, future);
        return;
    }

    if (!owned && gPriorAsyncRunHandler) {
        gPriorAsyncRunHandler(args, result);
        return;
    }
    void *future = owned ? rt_async_run_owned(entry, arg) : rt_async_run(entry, arg);
    publishAsyncFutureResult(result, future);
}

/// @brief Handle `Async.Run`; the argument is borrowed (native parity, VDOC-127).
/// @param args Runtime ABI slots containing `(entry, argument)`.
/// @param result Optional slot receiving the future handle.
static void unified_async_run_handler(void **args, void *result) {
    unified_async_run_impl(args, result, /*owned=*/false);
}

/// @brief Handle `Async.RunOwned`; the future owns the argument on every backend.
/// @param args Runtime ABI slots containing `(entry, owned argument)`.
/// @param result Optional slot receiving the future handle.
static void unified_async_run_owned_handler(void **args, void *result) {
    unified_async_run_impl(args, result, /*owned=*/true);
}

/// @brief Trap for Async callback variants without a managed bridge (VDOC-127).
/// @details The cancellable and map variants would otherwise fall through to
///          the native C implementation, which casts the opaque VM function
///          value to a native pointer — undefined behavior. Trap with a clear
///          message when either interpreted backend is active.
/// @param api Runtime API name included in the trap message.
/// @return `true` after trapping for an active interpreter; otherwise `false`.
static bool asyncCallbackUnsupportedOnVm(const char *api) {
    if (il::vm::activeVMInstance() || activeBytecodeVMInstance()) {
        rt_trap((std::string(api) +
                 ": callback execution is not supported on the interpreted backends; "
                 "compile to native to run it")
                    .c_str());
        return true;
    }
    return false;
}

/// @brief Dispatch `Async.RunCancellable`, rejecting interpreted callbacks.
/// @param args Runtime ABI slots containing `(entry, argument, token)`.
/// @param result Optional slot receiving the future handle.
static void unified_async_run_cancellable_handler(void **args, void *result) {
    if (asyncCallbackUnsupportedOnVm("Async.RunCancellable"))
        return;
    void *entry = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *arg = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *token = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *future = rt_async_run_cancellable(entry, arg, token);
    publishAsyncFutureResult(result, future);
}

/// @brief Dispatch owned `Async.RunCancellable`, rejecting interpreted callbacks.
/// @param args Runtime ABI slots containing `(entry, owned argument, token)`.
/// @param result Optional slot receiving the future handle.
static void unified_async_run_cancellable_owned_handler(void **args, void *result) {
    if (asyncCallbackUnsupportedOnVm("Async.RunCancellableOwned"))
        return;
    void *entry = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *arg = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *token = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *future = rt_async_run_cancellable_owned(entry, arg, token);
    publishAsyncFutureResult(result, future);
}

/// @brief Dispatch `Async.Map`, rejecting interpreted mapper callbacks.
/// @param args Runtime ABI slots containing `(future, mapper, argument)`.
/// @param result Optional slot receiving the mapped future.
static void unified_async_map_handler(void **args, void *result) {
    if (asyncCallbackUnsupportedOnVm("Async.Map"))
        return;
    void *future = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *fn = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *mapped = rt_async_map(future, fn, arg);
    publishAsyncFutureResult(result, mapped);
}

/// @brief Dispatch owned `Async.Map`, rejecting interpreted mapper callbacks.
/// @param args Runtime ABI slots containing `(future, mapper, owned argument)`.
/// @param result Optional slot receiving the mapped future.
static void unified_async_map_owned_handler(void **args, void *result) {
    if (asyncCallbackUnsupportedOnVm("Async.MapOwned"))
        return;
    void *future = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *fn = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *mapped = rt_async_map_owned(future, fn, arg);
    publishAsyncFutureResult(result, mapped);
}

/// @brief Run an index-range callback sequentially on the active BytecodeVM.
/// @details Parallel.For iterations are independent, so sequential reentrant execution yields the
///          same result as the native parallel path while keeping the bytecode function value
///          (a tagged module-function index, not native code) runnable. The callback must be
///          `(Integer) -> Unit`. A trapped iteration stops the loop.
/// @param vm Active VM used for re-entrant callback execution.
/// @param module Module used to resolve @p func.
/// @param start Inclusive first index.
/// @param end Exclusive final index.
/// @param func Tagged or raw bytecode callback value.
/// @param api Runtime API name included in traps.
static void runBytecodeParallelForRange(BytecodeVM &vm,
                                        const BytecodeModule &module,
                                        int64_t start,
                                        int64_t end,
                                        void *func,
                                        const char *api) {
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return;
    }
    if (fn->hasReturn || fn->numParams != 1) {
        rt_trap((std::string(api) + ": callback must be (Integer) -> Unit").c_str());
        return;
    }
    for (int64_t i = start; i < end; ++i) {
        std::vector<BCSlot> callArgs{BCSlot(i)};
        if (!vm.invokeVoidReentrant(fn, callArgs))
            return; // Trap recorded on the VM; stop the range.
    }
}

/// @brief Run a pool task callback synchronously on the active BytecodeVM.
/// @details The bytecode VM cannot dispatch its tagged function values onto native pool threads,
///          so the task runs immediately on the calling thread. The callback must be
///          `(Ptr) -> Unit` or take no parameters.
/// @param vm Active VM used for re-entrant execution.
/// @param module Module used to resolve @p callback.
/// @param callback Tagged or raw bytecode callback value.
/// @param arg Opaque argument forwarded when the callback accepts one parameter.
/// @param api Runtime API name included in traps.
static void runBytecodePoolTask(
    BytecodeVM &vm, const BytecodeModule &module, void *callback, void *arg, const char *api) {
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, callback);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return;
    }
    if (fn->hasReturn || fn->numParams > 1) {
        rt_trap((std::string(api) + ": callback must be (Ptr) -> Unit").c_str());
        return;
    }
    std::vector<BCSlot> taskArgs;
    if (fn->numParams == 1) {
        BCSlot s;
        s.ptr = arg;
        taskArgs.push_back(s);
    }
    vm.invokeVoidReentrant(fn, taskArgs);
}

/// @brief Invoke a sequence of zero-argument callbacks sequentially on the active BytecodeVM.
/// @details Each sequence element is a tagged bytecode function value; the actions run in order on
///          the calling thread. Callbacks must be `() -> Unit`. A trapped action stops the run.
/// @param vm Active VM used for re-entrant execution.
/// @param module Module used to resolve sequence entries.
/// @param funcs Runtime sequence of opaque callback values; null is a no-op.
/// @param api Runtime API name included in traps.
static void runBytecodeInvoke(BytecodeVM &vm,
                              const BytecodeModule &module,
                              void *funcs,
                              const char *api) {
    if (!funcs)
        return;
    const int64_t count = rt_seq_len(funcs);
    if (count < 0) {
        rt_trap((std::string(api) + ": negative sequence length").c_str());
        return;
    }
    for (int64_t i = 0; i < count; ++i) {
        const BytecodeFunction *fn = resolveBytecodeEntry(&module, rt_seq_get(funcs, i));
        if (!fn) {
            rt_trap((std::string(api) + ": invalid callback function").c_str());
            return;
        }
        if (fn->hasReturn || fn->numParams != 0) {
            rt_trap((std::string(api) + ": callbacks must be () -> Unit").c_str());
            return;
        }
        std::vector<BCSlot> noArgs;
        if (!vm.invokeVoidReentrant(fn, noArgs))
            return;
    }
}

/// @brief Bridge `Parallel.For` for the bytecode VM, otherwise chain or run natively.
/// @param args Runtime ABI slots containing `(start, end, callback)`.
/// @param result Unused void-result storage forwarded to a prior handler.
/// @details Bytecode callbacks run sequentially and re-entrantly; tree-walker
///          and native calls retain their registered behavior.
static void unified_parallel_for_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            int64_t start = args && args[0] ? *reinterpret_cast<int64_t *>(args[0]) : 0;
            int64_t end = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
            void *func = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
            runBytecodeParallelForRange(*bcVm, *bcModule, start, end, func, "Parallel.For");
            return;
        }
    }
    if (gPriorParallelForHandler) {
        gPriorParallelForHandler(args, result);
        return;
    }
    int64_t start = args && args[0] ? *reinterpret_cast<int64_t *>(args[0]) : 0;
    int64_t end = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
    void *func = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    rt_parallel_for(start, end, func);
}

/// @brief Bridge `Parallel.ForPool`, ignoring the pool for bytecode callbacks.
/// @param args Runtime ABI slots containing `(start, end, callback, pool)`.
/// @param result Unused void-result storage forwarded to a prior handler.
/// @details The bytecode range runs sequentially; other backends chain or use
///          the native pool.
static void unified_parallel_for_pool_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            int64_t start = args && args[0] ? *reinterpret_cast<int64_t *>(args[0]) : 0;
            int64_t end = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
            void *func = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
            runBytecodeParallelForRange(*bcVm, *bcModule, start, end, func, "Parallel.ForPool");
            return;
        }
    }
    if (gPriorParallelForPoolHandler) {
        gPriorParallelForPoolHandler(args, result);
        return;
    }
    int64_t start = args && args[0] ? *reinterpret_cast<int64_t *>(args[0]) : 0;
    int64_t end = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
    void *func = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *pool = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;
    rt_parallel_for_pool(start, end, func, pool);
}

/// @brief Bridge `Pool.Submit`, executing bytecode tasks synchronously.
/// @param args Runtime ABI slots containing `(pool, callback, argument)`.
/// @param result Optional Boolean slot receiving submission success.
/// @details A null or shut-down pool rejects the bytecode submission; other
///          backends chain or submit through the native runtime.
static void unified_pool_submit_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *pool = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
            // Pool state is honored on the VM too (VDOC-125): a shut-down or
            // invalid pool rejects the submission like the native backend.
            if (!pool || rt_threadpool_get_is_shutdown(pool)) {
                if (result)
                    *reinterpret_cast<int8_t *>(result) = 0;
                return;
            }
            runBytecodePoolTask(*bcVm, *bcModule, callback, arg, "Pool.Submit");
            if (result)
                *reinterpret_cast<int8_t *>(result) = 1;
            return;
        }
    }
    if (gPriorPoolSubmitHandler) {
        gPriorPoolSubmitHandler(args, result);
        return;
    }
    void *pool = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    int8_t submitted = rt_threadpool_submit(pool, callback, arg);
    if (result)
        *reinterpret_cast<int8_t *>(result) = submitted;
}

/// @brief SubmitOwned bridge: synchronous on the BytecodeVM, so the pool-side
///        retain/release nets out within the call (VDOC-128).
/// @param args Runtime ABI slots containing `(pool, callback, owned argument)`.
/// @param result Optional Boolean slot receiving submission success.
static void unified_pool_submit_owned_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *pool = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
            if (!pool || rt_threadpool_get_is_shutdown(pool)) {
                if (result)
                    *reinterpret_cast<int8_t *>(result) = 0;
                return;
            }
            runBytecodePoolTask(*bcVm, *bcModule, callback, arg, "Pool.SubmitOwned");
            if (result)
                *reinterpret_cast<int8_t *>(result) = 1;
            return;
        }
    }
    void *pool = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    int8_t submitted = rt_threadpool_submit_owned(pool, callback, arg);
    if (result)
        *reinterpret_cast<int8_t *>(result) = submitted;
}

/// @brief Bridge `Parallel.Invoke`, running bytecode actions sequentially.
/// @param args Runtime ABI slots containing a sequence of callbacks.
/// @param result Unused void-result storage forwarded to a prior handler.
static void unified_parallel_invoke_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *funcs = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            runBytecodeInvoke(*bcVm, *bcModule, funcs, "Parallel.Invoke");
            return;
        }
    }
    if (gPriorParallelInvokeHandler) {
        gPriorParallelInvokeHandler(args, result);
        return;
    }
    void *funcs = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    rt_parallel_invoke(funcs);
}

/// @brief Bridge `Parallel.InvokePool`, ignoring the pool for bytecode actions.
/// @param args Runtime ABI slots containing `(callbacks, pool)`.
/// @param result Unused void-result storage forwarded to a prior handler.
static void unified_parallel_invoke_pool_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *funcs = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            runBytecodeInvoke(*bcVm, *bcModule, funcs, "Parallel.InvokePool");
            return;
        }
    }
    if (gPriorParallelInvokePoolHandler) {
        gPriorParallelInvokePoolHandler(args, result);
        return;
    }
    void *funcs = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *pool = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    rt_parallel_invoke_pool(funcs, pool);
}

/// @brief Run a `(Ptr) -> Unit` callback over each Seq element (VDOC-126).
/// @details Sequential equivalent of the native Parallel.ForEach on the
///          BytecodeVM: same visible effects, single-threaded ordering.
/// @param vm Active VM used for re-entrant callback execution.
/// @param module Module used to resolve @p func.
/// @param seq Borrowed runtime sequence; null is a no-op.
/// @param func Tagged or raw bytecode callback value.
/// @param api Runtime API name included in traps.
static void runBytecodeSeqForEach(BytecodeVM &vm,
                                  const BytecodeModule &module,
                                  void *seq,
                                  void *func,
                                  const char *api) {
    if (!seq)
        return;
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return;
    }
    if (fn->hasReturn || fn->numParams != 1) {
        rt_trap((std::string(api) + ": callback must be (Object) -> Unit").c_str());
        return;
    }
    const int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        std::vector<BCSlot> callArgs(1);
        callArgs[0].ptr = rt_seq_get(seq, i);
        if (!vm.invokeVoidReentrant(fn, callArgs))
            return;
    }
}

/// @brief Map a Seq through a `(Ptr) -> Ptr` callback (VDOC-126); returns an
///        owning Seq of the mapped values in order.
/// @param vm Active VM used for re-entrant callback execution.
/// @param module Module used to resolve @p func.
/// @param seq Borrowed runtime sequence; null produces an empty sequence.
/// @param func Tagged or raw bytecode mapper value.
/// @param api Runtime API name included in traps.
/// @return Newly allocated owning runtime sequence.
static void *runBytecodeSeqMap(BytecodeVM &vm,
                               const BytecodeModule &module,
                               void *seq,
                               void *func,
                               const char *api) {
    void *out = rt_seq_new();
    rt_seq_set_owns_elements(out, 1);
    if (!seq)
        return out;
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return out;
    }
    if (!fn->hasReturn || fn->numParams != 1) {
        rt_trap((std::string(api) + ": callback must be (Object) -> Object").c_str());
        return out;
    }
    const int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        std::vector<BCSlot> callArgs(1);
        callArgs[0].ptr = rt_seq_get(seq, i);
        BCSlot r{};
        if (!vm.invokeValueReentrant(fn, callArgs, &r))
            return out;
        rt_seq_push(out, r.ptr);
    }
    return out;
}

/// @brief Left-fold a Seq through a `(Ptr, Ptr) -> Ptr` callback (VDOC-126).
/// @param vm Active VM used for re-entrant callback execution.
/// @param module Module used to resolve @p func.
/// @param seq Borrowed runtime sequence; null returns @p identity.
/// @param func Tagged or raw bytecode reducer value.
/// @param identity Initial accumulator.
/// @param api Runtime API name included in traps.
/// @return Final accumulator, or the last value produced before a trap.
static void *runBytecodeSeqReduce(BytecodeVM &vm,
                                  const BytecodeModule &module,
                                  void *seq,
                                  void *func,
                                  void *identity,
                                  const char *api) {
    if (!seq)
        return identity;
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return identity;
    }
    if (!fn->hasReturn || fn->numParams != 2) {
        rt_trap((std::string(api) + ": callback must be (Object, Object) -> Object").c_str());
        return identity;
    }
    void *acc = identity;
    const int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        std::vector<BCSlot> callArgs(2);
        callArgs[0].ptr = acc;
        callArgs[1].ptr = rt_seq_get(seq, i);
        BCSlot r{};
        if (!vm.invokeValueReentrant(fn, callArgs, &r))
            return acc;
        acc = r.ptr;
    }
    return acc;
}

/// @brief Bridge `Parallel.ForEach` with sequential bytecode callback execution.
/// @param args Runtime ABI slots containing `(sequence, callback)`.
/// @param result Unused void-result storage forwarded to a prior handler.
static void unified_parallel_foreach_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            runBytecodeSeqForEach(*bcVm, *bcModule, seq, func, "Parallel.ForEach");
            return;
        }
    }
    if (gPriorParallelForEachHandler) {
        gPriorParallelForEachHandler(args, result);
        return;
    }
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    rt_parallel_foreach(seq, func);
}

/// @brief Pool-taking counterpart of @ref unified_parallel_foreach_handler.
/// @param args Runtime ABI slots containing `(sequence, callback, pool)`.
/// @param result Unused void-result storage forwarded to a prior handler.
/// @details Bytecode execution is sequential and ignores the pool parameter.
static void unified_parallel_foreach_pool_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            runBytecodeSeqForEach(*bcVm, *bcModule, seq, func, "Parallel.ForEachPool");
            return;
        }
    }
    if (gPriorParallelForEachPoolHandler) {
        gPriorParallelForEachPoolHandler(args, result);
        return;
    }
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *pool = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    rt_parallel_foreach_pool(seq, func, pool);
}

/// @brief Bridge `Parallel.Map` with sequential bytecode mapper execution.
/// @param args Runtime ABI slots containing `(sequence, mapper)`.
/// @param result Optional slot receiving the newly allocated mapped sequence.
static void unified_parallel_map_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            void *mapped = runBytecodeSeqMap(*bcVm, *bcModule, seq, func, "Parallel.Map");
            if (result)
                *reinterpret_cast<void **>(result) = mapped;
            return;
        }
    }
    if (gPriorParallelMapHandler) {
        gPriorParallelMapHandler(args, result);
        return;
    }
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *mapped = rt_parallel_map(seq, func);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Pool-taking counterpart of @ref unified_parallel_map_handler.
/// @param args Runtime ABI slots containing `(sequence, mapper, pool)`.
/// @param result Optional slot receiving the newly allocated mapped sequence.
/// @details Bytecode execution is sequential and ignores the pool parameter.
static void unified_parallel_map_pool_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            void *mapped = runBytecodeSeqMap(*bcVm, *bcModule, seq, func, "Parallel.MapPool");
            if (result)
                *reinterpret_cast<void **>(result) = mapped;
            return;
        }
    }
    if (gPriorParallelMapPoolHandler) {
        gPriorParallelMapPoolHandler(args, result);
        return;
    }
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *pool = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *mapped = rt_parallel_map_pool(seq, func, pool);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Bridge `Parallel.Reduce` with a sequential bytecode left fold.
/// @param args Runtime ABI slots containing `(sequence, reducer, identity)`.
/// @param result Optional slot receiving the final accumulator.
static void unified_parallel_reduce_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            void *identity = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
            void *reduced =
                runBytecodeSeqReduce(*bcVm, *bcModule, seq, func, identity, "Parallel.Reduce");
            if (result)
                *reinterpret_cast<void **>(result) = reduced;
            return;
        }
    }
    if (gPriorParallelReduceHandler) {
        gPriorParallelReduceHandler(args, result);
        return;
    }
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *identity = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *reduced = rt_parallel_reduce(seq, func, identity);
    if (result)
        *reinterpret_cast<void **>(result) = reduced;
}

/// @brief Pool-taking counterpart of @ref unified_parallel_reduce_handler.
/// @param args Runtime ABI slots containing `(sequence, reducer, identity, pool)`.
/// @param result Optional slot receiving the final accumulator.
/// @details Bytecode execution is sequential and ignores the pool parameter.
static void unified_parallel_reduce_pool_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            void *identity = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
            void *reduced = runBytecodeSeqReduce(
                *bcVm, *bcModule, seq, func, identity, "Parallel.ReducePool");
            if (result)
                *reinterpret_cast<void **>(result) = reduced;
            return;
        }
    }
    if (gPriorParallelReducePoolHandler) {
        gPriorParallelReducePoolHandler(args, result);
        return;
    }
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *identity = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *pool = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;
    void *reduced = rt_parallel_reduce_pool(seq, func, identity, pool);
    if (result)
        *reinterpret_cast<void **>(result) = reduced;
}

//===----------------------------------------------------------------------===//
// Zanna.Functional.Lazy handlers (deferred suppliers and combinator callbacks)
//===----------------------------------------------------------------------===//

/// @brief Execute a zero-argument, value-returning bytecode supplier reentrantly.
/// @param vm Active VM used for re-entrant execution.
/// @param module Module used to resolve @p handle.
/// @param handle Tagged or raw supplier function value.
/// @param api Runtime API name included in traps.
/// @return The supplier's object result, or null after a trap.
static void *runBytecodeSupplier(BytecodeVM &vm,
                                 const BytecodeModule &module,
                                 void *handle,
                                 const char *api) {
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, handle);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid supplier function").c_str());
        return nullptr;
    }
    if (!fn->hasReturn || fn->numParams != 0) {
        rt_trap((std::string(api) + ": supplier must be () -> Object").c_str());
        return nullptr;
    }
    BCSlot out{};
    if (!vm.invokeValueReentrant(fn, {}, &out))
        return nullptr;
    return out.ptr;
}

/// @brief Execute a one-argument, value-returning bytecode callback reentrantly.
/// @param vm Active VM used for re-entrant execution.
/// @param module Module used to resolve @p callback.
/// @param callback Tagged or raw bytecode function value.
/// @param value Object passed to the callback.
/// @param api Runtime API name included in traps.
/// @return The callback's object result, or null after a trap.
static void *runBytecodeCallback1(
    BytecodeVM &vm, const BytecodeModule &module, void *callback, void *value, const char *api) {
    const BytecodeFunction *fn = resolveBytecodeEntry(&module, callback);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return nullptr;
    }
    if (!fn->hasReturn || fn->numParams != 1) {
        rt_trap((std::string(api) + ": callback must be (Object) -> Object").c_str());
        return nullptr;
    }
    BCSlot arg{};
    arg.ptr = value;
    BCSlot out{};
    if (!vm.invokeValueReentrant(fn, {arg}, &out))
        return nullptr;
    return out.ptr;
}

/// @brief Run a pending handle-kind Lazy supplier on the active BytecodeVM.
/// @param vm Active VM used for re-entrant supplier execution.
/// @param module Module used to resolve a pending supplier handle.
/// @param lazy Runtime Lazy object; unchanged if already complete.
/// @param api Runtime API name included in traps.
static void completeBytecodePendingLazy(BytecodeVM &vm,
                                        const BytecodeModule &module,
                                        void *lazy,
                                        const char *api) {
    if (void *handle = rt_lazy_pending_handle(lazy)) {
        void *value = runBytecodeSupplier(vm, module, handle, api);
        rt_lazy_complete_obj(lazy, value);
    }
}

/// @brief Bridge `Lazy.New`, preserving bytecode suppliers as opaque handles.
/// @param args Runtime ABI slots containing the supplier.
/// @param result Optional slot receiving the new Lazy object.
/// @details A bytecode supplier is validated and deferred for re-entrant
///          execution; other backends chain or use the native wrapper.
static void unified_lazy_new_handler(void **args, void *result) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *supplier = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            const BytecodeFunction *fn = resolveBytecodeEntry(bcModule, supplier);
            if (!fn) {
                rt_trap("Lazy.New: invalid supplier function");
                return;
            }
            if (!fn->hasReturn || fn->numParams != 0) {
                rt_trap("Lazy.New: supplier must be () -> Object");
                return;
            }
            (void)bcVm;
            void *lazy = rt_lazy_new_handle(supplier);
            if (result)
                *reinterpret_cast<void **>(result) = lazy;
            return;
        }
    }
    if (gPriorLazyNewHandler) {
        gPriorLazyNewHandler(args, result);
        return;
    }
    void *supplier = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *lazy = rt_lazy_new_wrapper(supplier);
    if (result)
        *reinterpret_cast<void **>(result) = lazy;
}

/// @brief Bridge `Lazy.Get`, completing a pending bytecode supplier first.
/// @param args Runtime ABI slots containing the Lazy object.
/// @param result Optional slot receiving its object value.
static void unified_lazy_get_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            completeBytecodePendingLazy(*bcVm, *bcModule, lazy, "Lazy.Get");
            void *value = rt_lazy_get(lazy);
            if (result)
                *reinterpret_cast<void **>(result) = value;
            return;
        }
    }
    if (gPriorLazyGetHandler) {
        gPriorLazyGetHandler(args, result);
        return;
    }
    void *value = rt_lazy_get(lazy);
    if (result)
        *reinterpret_cast<void **>(result) = value;
}

/// @brief Bridge `Lazy.GetStr`, completing a pending bytecode supplier first.
/// @param args Runtime ABI slots containing the Lazy object.
/// @param result Optional slot receiving its runtime string value.
static void unified_lazy_get_str_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            completeBytecodePendingLazy(*bcVm, *bcModule, lazy, "Lazy.GetStr");
            rt_string value = rt_lazy_get_str(lazy);
            if (result)
                *reinterpret_cast<rt_string *>(result) = value;
            return;
        }
    }
    if (gPriorLazyGetStrHandler) {
        gPriorLazyGetStrHandler(args, result);
        return;
    }
    rt_string value = rt_lazy_get_str(lazy);
    if (result)
        *reinterpret_cast<rt_string *>(result) = value;
}

/// @brief Bridge `Lazy.GetI64`, completing a pending bytecode supplier first.
/// @param args Runtime ABI slots containing the Lazy object.
/// @param result Optional slot receiving its integer value.
static void unified_lazy_get_i64_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            completeBytecodePendingLazy(*bcVm, *bcModule, lazy, "Lazy.GetI64");
            int64_t value = rt_lazy_get_i64(lazy);
            if (result)
                *reinterpret_cast<int64_t *>(result) = value;
            return;
        }
    }
    if (gPriorLazyGetI64Handler) {
        gPriorLazyGetI64Handler(args, result);
        return;
    }
    int64_t value = rt_lazy_get_i64(lazy);
    if (result)
        *reinterpret_cast<int64_t *>(result) = value;
}

/// @brief Bridge `Lazy.Force`, completing a pending bytecode supplier first.
/// @param args Runtime ABI slots containing the Lazy object.
/// @param result Unused void-result storage forwarded to a prior handler.
static void unified_lazy_force_handler(void **args, void *result) {
    (void)result;
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            completeBytecodePendingLazy(*bcVm, *bcModule, lazy, "Lazy.Force");
            rt_lazy_force(lazy);
            return;
        }
    }
    if (gPriorLazyForceHandler) {
        gPriorLazyForceHandler(args, result);
        return;
    }
    rt_lazy_force(lazy);
}

/// @brief Bridge `Lazy.Map` for bytecode callbacks.
/// @param args Runtime ABI slots containing `(lazy, mapper)`.
/// @param result Optional slot receiving the mapped Lazy object.
/// @details Mirrors native semantics by forcing the source, applying the
///          mapper re-entrantly, and wrapping the transformed value.
static void unified_lazy_map_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *mapped = nullptr;
            if (lazy && callback) {
                completeBytecodePendingLazy(*bcVm, *bcModule, lazy, "Lazy.Map");
                void *value = rt_lazy_get(lazy);
                void *transformed =
                    runBytecodeCallback1(*bcVm, *bcModule, callback, value, "Lazy.Map");
                mapped = rt_lazy_of(transformed);
            } else {
                mapped = lazy;
            }
            if (result)
                *reinterpret_cast<void **>(result) = mapped;
            return;
        }
    }
    if (gPriorLazyMapHandler) {
        gPriorLazyMapHandler(args, result);
        return;
    }
    void *mapped = rt_lazy_map_wrapper(lazy, callback);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Bridge `Lazy.AndThen` for bytecode callbacks.
/// @param args Runtime ABI slots containing `(lazy, callback)`.
/// @param result Optional slot receiving the callback's Lazy object.
/// @details Forces the source and returns the callback result directly rather
///          than wrapping it in another Lazy.
static void unified_lazy_and_then_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *chained = nullptr;
            if (lazy && callback) {
                completeBytecodePendingLazy(*bcVm, *bcModule, lazy, "Lazy.AndThen");
                void *value = rt_lazy_get(lazy);
                chained = runBytecodeCallback1(*bcVm, *bcModule, callback, value, "Lazy.AndThen");
            } else {
                chained = lazy;
            }
            if (result)
                *reinterpret_cast<void **>(result) = chained;
            return;
        }
    }
    if (gPriorLazyAndThenHandler) {
        gPriorLazyAndThenHandler(args, result);
        return;
    }
    void *chained = rt_lazy_flat_map_wrapper(lazy, callback);
    if (result)
        *reinterpret_cast<void **>(result) = chained;
}

//===----------------------------------------------------------------------===//
// Zanna.Option / Zanna.Result combinator handlers
//===----------------------------------------------------------------------===//

/// @brief Context threaded through the rt_cb_invoke* strategies for bytecode execution.
struct BcInvokerCtx {
    /// Borrowed active VM.
    BytecodeVM *vm;
    /// Borrowed active module.
    const BytecodeModule *module;
    /// API name used in trap diagnostics.
    const char *api;
};

/// @brief rt_cb_invoke1 strategy executing the callback on the active BytecodeVM.
/// @param ctxRaw Borrowed `BcInvokerCtx`.
/// @param fn Tagged or raw one-argument callback.
/// @param arg Object forwarded to @p fn.
/// @return Callback result object, or null after a trap.
static void *bcInvoke1(void *ctxRaw, void *fn, void *arg) {
    auto *ctx = static_cast<BcInvokerCtx *>(ctxRaw);
    return runBytecodeCallback1(*ctx->vm, *ctx->module, fn, arg, ctx->api);
}

/// @brief rt_cb_invoke0 strategy executing the callback on the active BytecodeVM.
/// @param ctxRaw Borrowed `BcInvokerCtx`.
/// @param fn Tagged or raw zero-argument supplier.
/// @return Supplier result object, or null after a trap.
static void *bcInvoke0(void *ctxRaw, void *fn) {
    auto *ctx = static_cast<BcInvokerCtx *>(ctxRaw);
    return runBytecodeSupplier(*ctx->vm, *ctx->module, fn, ctx->api);
}

/// @brief rt_cb_invoke_pred strategy executing the predicate on the active BytecodeVM.
/// @param ctxRaw Borrowed `BcInvokerCtx`.
/// @param fn Tagged or raw one-argument predicate.
/// @param arg Object forwarded to @p fn.
/// @return One for a truthy callback result, otherwise zero.
static int8_t bcInvokePred(void *ctxRaw, void *fn, void *arg) {
    auto *ctx = static_cast<BcInvokerCtx *>(ctxRaw);
    const BytecodeFunction *callback = resolveBytecodeEntry(ctx->module, fn);
    if (!callback) {
        rt_trap((std::string(ctx->api) + ": invalid predicate function").c_str());
        return 0;
    }
    if (!callback->hasReturn || callback->numParams != 1) {
        rt_trap((std::string(ctx->api) + ": predicate must be (Object) -> Boolean").c_str());
        return 0;
    }
    BCSlot argSlot{};
    argSlot.ptr = arg;
    BCSlot out{};
    if (!ctx->vm->invokeValueReentrant(callback, {argSlot}, &out))
        return 0;
    return out.i64 != 0 ? 1 : 0;
}

/// @brief Shared body for the eight Option/Result combinator handlers.
/// @details Runs the combinator core with a bytecode invoker when a BytecodeVM is
///          active; otherwise chains to the prior handler or the native wrapper.
/// @tparam CoreFn Callable type for the strategy-aware runtime core.
/// @tparam Strategy Callback-invocation strategy type.
/// @tparam NativeFn Callable type for the native wrapper fallback.
/// @param args Runtime ABI slots containing `(receiver, callback)`.
/// @param result Optional slot receiving the combined Option or Result.
/// @param api Runtime API name used by bytecode callback traps.
/// @param core Strategy-aware combinator implementation.
/// @param strategy Bytecode callback invocation strategy.
/// @param native Native callback wrapper used outside an interpreter.
/// @param prior Previously registered runtime handler, if any.
template <typename CoreFn, typename Strategy, typename NativeFn>
static void dispatchBytecodeCombinator(void **args,
                                       void *result,
                                       const char *api,
                                       CoreFn core,
                                       Strategy strategy,
                                       NativeFn native,
                                       UnifiedRuntimeHandler prior) {
    if (BytecodeVM *bcVm = activeBytecodeVMInstance()) {
        if (const BytecodeModule *bcModule = activeBytecodeModule()) {
            void *receiver = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
            void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
            BcInvokerCtx ctx{bcVm, bcModule, api};
            void *combined = core(receiver, callback, strategy, &ctx);
            if (result)
                *reinterpret_cast<void **>(result) = combined;
            return;
        }
    }
    if (prior) {
        prior(args, result);
        return;
    }
    void *receiver = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *combined = native(receiver, callback);
    if (result)
        *reinterpret_cast<void **>(result) = combined;
}

/// @brief Bridge `Option.Map` through the active bytecode callback strategy.
/// @param args Runtime ABI slots containing `(option, mapper)`.
/// @param result Optional slot receiving the mapped Option.
static void unified_option_map_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Option.Map",
                               &rt_option_map_invoke,
                               &bcInvoke1,
                               &rt_option_map_wrapper,
                               gPriorOptionMapHandler);
}

/// @brief Bridge `Option.AndThen` through the active bytecode callback strategy.
/// @param args Runtime ABI slots containing `(option, callback)`.
/// @param result Optional slot receiving the flattened Option.
static void unified_option_and_then_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Option.AndThen",
                               &rt_option_and_then_invoke,
                               &bcInvoke1,
                               &rt_option_and_then_wrapper,
                               gPriorOptionAndThenHandler);
}

/// @brief Bridge `Option.OrElse` through the active bytecode supplier strategy.
/// @param args Runtime ABI slots containing `(option, fallback supplier)`.
/// @param result Optional slot receiving the selected Option.
static void unified_option_or_else_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Option.OrElse",
                               &rt_option_or_else_invoke,
                               &bcInvoke0,
                               &rt_option_or_else_wrapper,
                               gPriorOptionOrElseHandler);
}

/// @brief Bridge `Option.Filter` through the active bytecode predicate strategy.
/// @param args Runtime ABI slots containing `(option, predicate)`.
/// @param result Optional slot receiving the filtered Option.
static void unified_option_filter_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Option.Filter",
                               &rt_option_filter_invoke,
                               &bcInvokePred,
                               &rt_option_filter_wrapper,
                               gPriorOptionFilterHandler);
}

/// @brief Bridge `Result.Map` through the active bytecode callback strategy.
/// @param args Runtime ABI slots containing `(result, mapper)`.
/// @param result Optional slot receiving the mapped Result.
static void unified_result_map_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Result.Map",
                               &rt_result_map_invoke,
                               &bcInvoke1,
                               &rt_result_map_wrapper,
                               gPriorResultMapHandler);
}

/// @brief Bridge `Result.MapErr` through the active bytecode callback strategy.
/// @param args Runtime ABI slots containing `(result, error mapper)`.
/// @param result Optional slot receiving the mapped Result.
static void unified_result_map_err_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Result.MapErr",
                               &rt_result_map_err_invoke,
                               &bcInvoke1,
                               &rt_result_map_err_wrapper,
                               gPriorResultMapErrHandler);
}

/// @brief Bridge `Result.AndThen` through the active bytecode callback strategy.
/// @param args Runtime ABI slots containing `(result, callback)`.
/// @param result Optional slot receiving the flattened Result.
static void unified_result_and_then_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Result.AndThen",
                               &rt_result_and_then_invoke,
                               &bcInvoke1,
                               &rt_result_and_then_wrapper,
                               gPriorResultAndThenHandler);
}

/// @brief Bridge `Result.OrElse` through the active bytecode callback strategy.
/// @param args Runtime ABI slots containing `(result, recovery callback)`.
/// @param result Optional slot receiving the recovered Result.
static void unified_result_or_else_handler(void **args, void *result) {
    dispatchBytecodeCombinator(args,
                               result,
                               "Result.OrElse",
                               &rt_result_or_else_invoke,
                               &bcInvoke1,
                               &rt_result_or_else_wrapper,
                               gPriorResultOrElseHandler);
}

/// @brief Install the dual-engine runtime handlers for callback-taking runtime APIs.
/// @details Captures and chains to any previously registered Thread, Async,
///          Network, Game3D, Parallel, Lazy, Option, and Result handlers.
///          Registration is idempotent via `std::call_once`; @ref load and
///          @ref BytecodeVM::exec invoke it before bytecode can call a runtime
///          API with an interpreted callback.
void registerUnifiedVmRuntimeHandlers() {
    /// @brief Capture prior handlers and install all unified runtime bridges exactly once.
    std::call_once(gUnifiedRuntimeHandlersOnce, []() {
        using il::runtime::signatures::make_signature;
        using il::runtime::signatures::SigParam;

        /// @brief Preserve an existing non-self handler for calls outside an interpreted VM.
        /// @param name Runtime external name to inspect.
        /// @param currentFn Unified handler being installed.
        /// @param[out] outHandler Receives the previously registered handler when distinct.
        auto capturePriorHandler =
            [](std::string_view name, void *currentFn, UnifiedRuntimeHandler &outHandler) {
                if (const il::vm::ExternDesc *existing = il::vm::RuntimeBridge::findExtern(name)) {
                    if (existing->fn != currentFn)
                        outHandler = reinterpret_cast<UnifiedRuntimeHandler>(existing->fn);
                }
            };

        capturePriorHandler("Zanna.Threads.Thread.Start",
                            reinterpret_cast<void *>(&unified_thread_start_handler),
                            gPriorThreadStartHandler);
        capturePriorHandler("Zanna.Threads.Thread.StartOwned",
                            reinterpret_cast<void *>(&unified_thread_start_owned_handler),
                            gPriorThreadStartOwnedHandler);
        capturePriorHandler("Zanna.Threads.Thread.StartSafe",
                            reinterpret_cast<void *>(&unified_thread_start_safe_handler),
                            gPriorThreadStartSafeHandler);
        capturePriorHandler("Zanna.Threads.Thread.StartSafeOwned",
                            reinterpret_cast<void *>(&unified_thread_start_safe_owned_handler),
                            gPriorThreadStartSafeOwnedHandler);
        capturePriorHandler("Zanna.Threads.Pool.SubmitOwned",
                            reinterpret_cast<void *>(&unified_pool_submit_owned_handler),
                            gPriorPoolSubmitOwnedHandler);
        capturePriorHandler("Zanna.Threads.Async.Run",
                            reinterpret_cast<void *>(&unified_async_run_handler),
                            gPriorAsyncRunHandler);
        capturePriorHandler("Zanna.Network.HttpServer.BindHandler",
                            reinterpret_cast<void *>(&unified_http_server_bind_handler),
                            gPriorHttpBindHandler);
        capturePriorHandler("Zanna.Network.HttpsServer.BindHandler",
                            reinterpret_cast<void *>(&unified_https_server_bind_handler),
                            gPriorHttpsBindHandler);
        capturePriorHandler("Zanna.Game3D.World3D.Run",
                            reinterpret_cast<void *>(&unified_game3d_run_handler),
                            gPriorGame3DRunHandler);
        capturePriorHandler("Zanna.Game3D.World3D.RunWithOverlay",
                            reinterpret_cast<void *>(&unified_game3d_run_with_overlay_handler),
                            gPriorGame3DRunWithOverlayHandler);
        capturePriorHandler("Zanna.Game3D.World3D.RunFixed",
                            reinterpret_cast<void *>(&unified_game3d_run_fixed_handler),
                            gPriorGame3DRunFixedHandler);
        capturePriorHandler(
            "Zanna.Game3D.World3D.RunFixedWithOverlay",
            reinterpret_cast<void *>(&unified_game3d_run_fixed_with_overlay_handler),
            gPriorGame3DRunFixedWithOverlayHandler);
        capturePriorHandler("Zanna.Game3D.World3D.RunFrames",
                            reinterpret_cast<void *>(&unified_game3d_run_frames_handler),
                            gPriorGame3DRunFramesHandler);
        capturePriorHandler("Zanna.Game3D.World3D.DrawOverlay",
                            reinterpret_cast<void *>(&unified_game3d_draw_overlay_handler),
                            gPriorGame3DDrawOverlayHandler);
        capturePriorHandler("Zanna.Threads.Parallel.For",
                            reinterpret_cast<void *>(&unified_parallel_for_handler),
                            gPriorParallelForHandler);
        capturePriorHandler("Zanna.Threads.Parallel.ForPool",
                            reinterpret_cast<void *>(&unified_parallel_for_pool_handler),
                            gPriorParallelForPoolHandler);
        capturePriorHandler("Zanna.Threads.Pool.Submit",
                            reinterpret_cast<void *>(&unified_pool_submit_handler),
                            gPriorPoolSubmitHandler);
        capturePriorHandler("Zanna.Threads.Parallel.Invoke",
                            reinterpret_cast<void *>(&unified_parallel_invoke_handler),
                            gPriorParallelInvokeHandler);
        capturePriorHandler("Zanna.Threads.Parallel.InvokePool",
                            reinterpret_cast<void *>(&unified_parallel_invoke_pool_handler),
                            gPriorParallelInvokePoolHandler);
        capturePriorHandler("Zanna.Threads.Parallel.ForEach",
                            reinterpret_cast<void *>(&unified_parallel_foreach_handler),
                            gPriorParallelForEachHandler);
        capturePriorHandler("Zanna.Threads.Parallel.ForEachPool",
                            reinterpret_cast<void *>(&unified_parallel_foreach_pool_handler),
                            gPriorParallelForEachPoolHandler);
        capturePriorHandler("Zanna.Threads.Parallel.Map",
                            reinterpret_cast<void *>(&unified_parallel_map_handler),
                            gPriorParallelMapHandler);
        capturePriorHandler("Zanna.Threads.Parallel.MapPool",
                            reinterpret_cast<void *>(&unified_parallel_map_pool_handler),
                            gPriorParallelMapPoolHandler);
        capturePriorHandler("Zanna.Threads.Parallel.Reduce",
                            reinterpret_cast<void *>(&unified_parallel_reduce_handler),
                            gPriorParallelReduceHandler);
        capturePriorHandler("Zanna.Threads.Parallel.ReducePool",
                            reinterpret_cast<void *>(&unified_parallel_reduce_pool_handler),
                            gPriorParallelReducePoolHandler);
        capturePriorHandler("Zanna.Functional.Lazy.New",
                            reinterpret_cast<void *>(&unified_lazy_new_handler),
                            gPriorLazyNewHandler);
        capturePriorHandler("Zanna.Functional.Lazy.Get",
                            reinterpret_cast<void *>(&unified_lazy_get_handler),
                            gPriorLazyGetHandler);
        capturePriorHandler("Zanna.Functional.Lazy.GetStr",
                            reinterpret_cast<void *>(&unified_lazy_get_str_handler),
                            gPriorLazyGetStrHandler);
        capturePriorHandler("Zanna.Functional.Lazy.GetI64",
                            reinterpret_cast<void *>(&unified_lazy_get_i64_handler),
                            gPriorLazyGetI64Handler);
        capturePriorHandler("Zanna.Functional.Lazy.Force",
                            reinterpret_cast<void *>(&unified_lazy_force_handler),
                            gPriorLazyForceHandler);
        capturePriorHandler("Zanna.Functional.Lazy.Map",
                            reinterpret_cast<void *>(&unified_lazy_map_handler),
                            gPriorLazyMapHandler);
        capturePriorHandler("Zanna.Functional.Lazy.AndThen",
                            reinterpret_cast<void *>(&unified_lazy_and_then_handler),
                            gPriorLazyAndThenHandler);
        capturePriorHandler("Zanna.Option.Map",
                            reinterpret_cast<void *>(&unified_option_map_handler),
                            gPriorOptionMapHandler);
        capturePriorHandler("Zanna.Option.AndThen",
                            reinterpret_cast<void *>(&unified_option_and_then_handler),
                            gPriorOptionAndThenHandler);
        capturePriorHandler("Zanna.Option.OrElse",
                            reinterpret_cast<void *>(&unified_option_or_else_handler),
                            gPriorOptionOrElseHandler);
        capturePriorHandler("Zanna.Option.Filter",
                            reinterpret_cast<void *>(&unified_option_filter_handler),
                            gPriorOptionFilterHandler);
        capturePriorHandler("Zanna.Result.Map",
                            reinterpret_cast<void *>(&unified_result_map_handler),
                            gPriorResultMapHandler);
        capturePriorHandler("Zanna.Result.MapErr",
                            reinterpret_cast<void *>(&unified_result_map_err_handler),
                            gPriorResultMapErrHandler);
        capturePriorHandler("Zanna.Result.AndThen",
                            reinterpret_cast<void *>(&unified_result_and_then_handler),
                            gPriorResultAndThenHandler);
        capturePriorHandler("Zanna.Result.OrElse",
                            reinterpret_cast<void *>(&unified_result_or_else_handler),
                            gPriorResultOrElseHandler);
    });

    using il::runtime::signatures::make_signature;
    using il::runtime::signatures::SigParam;

    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Thread.Start";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_thread_start_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Thread.StartOwned";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_thread_start_owned_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Thread.StartSafe";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_thread_start_safe_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Thread.StartSafeOwned";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_thread_start_safe_owned_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Async.Run";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_async_run_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Async.RunOwned";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_async_run_owned_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Async.RunCancellable";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_async_run_cancellable_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Async.RunCancellableOwned";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_async_run_cancellable_owned_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Async.Map";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_async_map_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Async.MapOwned";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_async_map_owned_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Network.HttpServer.BindHandler";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Str, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_http_server_bind_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Network.HttpsServer.BindHandler";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Str, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_https_server_bind_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Game3D.World3D.Run";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_game3d_run_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Game3D.World3D.RunWithOverlay";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_game3d_run_with_overlay_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Game3D.World3D.RunFixed";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::F64, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_game3d_run_fixed_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Game3D.World3D.RunFixedWithOverlay";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::F64, SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_game3d_run_fixed_with_overlay_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Game3D.World3D.RunFrames";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::I64, SigParam::F64, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_game3d_run_frames_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Game3D.World3D.DrawOverlay";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_game3d_draw_overlay_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.For";
        ext.signature = make_signature(ext.name, {SigParam::I64, SigParam::I64, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_for_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ForPool";
        ext.signature =
            make_signature(ext.name, {SigParam::I64, SigParam::I64, SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_for_pool_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Pool.Submit";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::I1});
        ext.fn = reinterpret_cast<void *>(&unified_pool_submit_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Pool.SubmitOwned";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::I1});
        ext.fn = reinterpret_cast<void *>(&unified_pool_submit_owned_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.Invoke";
        ext.signature = make_signature(ext.name, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_invoke_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.InvokePool";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_invoke_pool_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ForEach";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_foreach_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ForEachPool";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_foreach_pool_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.Map";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_map_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.MapPool";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_map_pool_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.Reduce";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_reduce_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ReducePool";
        ext.signature = make_signature(ext.name,
                                       {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr, SigParam::Ptr},
                                       {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_parallel_reduce_pool_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.New";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_new_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.Get";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_get_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.GetStr";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::Str});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_get_str_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.GetI64";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::I64});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_get_i64_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.Force";
        ext.signature = make_signature(ext.name, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_force_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.Map";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_map_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
    {
        il::vm::ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.AndThen";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&unified_lazy_and_then_handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }

    struct CombinatorExtern {
        const char *name;
        void (*handler)(void **, void *);
    };
    static constexpr CombinatorExtern kCombinators[] = {
        {"Zanna.Option.Map", &unified_option_map_handler},
        {"Zanna.Option.AndThen", &unified_option_and_then_handler},
        {"Zanna.Option.OrElse", &unified_option_or_else_handler},
        {"Zanna.Option.Filter", &unified_option_filter_handler},
        {"Zanna.Result.Map", &unified_result_map_handler},
        {"Zanna.Result.MapErr", &unified_result_map_err_handler},
        {"Zanna.Result.AndThen", &unified_result_and_then_handler},
        {"Zanna.Result.OrElse", &unified_result_or_else_handler},
    };
    for (const auto &entry : kCombinators) {
        il::vm::ExternDesc ext;
        ext.name = entry.name;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(entry.handler);
        il::vm::RuntimeBridge::registerExtern(ext);
    }
}

/// @brief Registers unified callback-taking handlers during static initialization.
/// @details Construction replaces the standard VM registrations with bytecode
///          bridge handlers before the containing library begins serving calls.
struct UnifiedThreadHandlerRegistrar {
    /// @brief Install every unified VM runtime handler in the shared registry.
    UnifiedThreadHandlerRegistrar() {
        registerUnifiedVmRuntimeHandlers();
    }
};

// Register the unified handlers when the library is loaded.
[[maybe_unused]] const UnifiedThreadHandlerRegistrar kUnifiedThreadHandlerRegistrar{};

} // anonymous namespace

} // namespace bytecode
} // namespace zanna
