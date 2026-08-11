//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/RuntimeBridge.cpp
// Purpose: Provide the glue between the Zanna VM and the C runtime library.
// Key invariants: The bridge maintains thread-local trap context and validates
//                 runtime signatures before invocation.
// Ownership/Lifetime: Bridge does not own VM or runtime resources.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the runtime bridge that dispatches IL runtime calls.
/// @details The bridge validates call arity, marshals VM slots into native
///          representations, invokes runtime thunks, and translates traps back
///          into VM errors.  It also exposes entry points used by the C runtime
///          to signal asynchronous traps into the active VM context.

#include "vm/RuntimeBridge.hpp"
#include "il/core/Opcode.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "rt_error.h"
#include "rt_gc.h"
#include "vm/DiagFormat.hpp"
#include "vm/Marshal.hpp"
#include "vm/OpcodeHandlerHelpers.hpp"
#include "vm/TrapInvariants.hpp"
#include "vm/VM.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using il::support::SourceLoc;

namespace {
using il::core::Opcode;
using il::runtime::RtSig;
using il::runtime::RuntimeDescriptor;
using il::vm::ExternDesc;
using il::vm::FrameInfo;
using il::vm::ResultBuffers;
using il::vm::RuntimeBridge;
using il::vm::RuntimeCallContext;
using il::vm::RuntimeTrapInterceptor;
using il::vm::RuntimeTrapSignal;
using il::vm::Slot;
using il::vm::TrapKind;
using il::vm::VM;
using il::vm::vm_format_error;
using il::vm::vm_raise_from_error;
using il::vm::VmError;

/// @brief Thread-local pointer to the runtime call context for active trap reporting.
///
/// The bridge stores the most recent call's context so asynchronous traps raised
/// from the C runtime can report diagnostics against the correct function and
/// source location.  The pointer is managed via @ref ContextGuard to ensure
/// balanced updates.
thread_local RuntimeCallContext *tlsContext = nullptr;
/// @brief Optional thread-local observer invoked before a runtime trap escalates.
thread_local RuntimeTrapInterceptor tlsTrapInterceptor = nullptr;
/// @brief Opaque caller data paired with @ref tlsTrapInterceptor.
thread_local void *tlsTrapInterceptorUserData = nullptr;

/// @brief VM slot returned by a runtime dispatch thunk.
using VmResult = Slot;
/// @brief Uniform function type used by the runtime signature thunk table.
/// @param vm Active virtual machine.
/// @param frame Runtime call-frame metadata.
/// @param context Call-site and diagnostic context.
/// @return Slot containing the marshalled runtime result.
using Thunk = VmResult (*)(VM &, FrameInfo &, const RuntimeCallContext &);

/// @brief Verify that a runtime call supplies the expected number of arguments.
///
/// @details Compares the descriptor's signature against the arguments assembled
///          by the VM.  Mismatches trigger a domain-error trap describing the
///          offending call site. Uses centralized marshalling validation helper.
///
/// @param desc Runtime descriptor describing the callee signature.
/// @param args Slots supplied by the VM as call arguments.
/// @param loc Source location associated with the call.
/// @param fn Name of the function executing the call.
/// @param block Name of the basic block executing the call.
/// @return True when counts match; false when a trap was raised.
static bool validateArgumentCount(const RuntimeDescriptor &desc,
                                  std::span<const Slot> args,
                                  const SourceLoc &loc,
                                  const std::string &fn,
                                  const std::string &block) {
    auto validation = il::vm::validateMarshalArity(desc, args.size());
    if (validation.ok)
        return true;

    RuntimeBridge::trap(TrapKind::DomainError, validation.errorMessage, loc, fn, block);
    return false;
}

/// @brief Execute a runtime descriptor by marshalling arguments and collecting results.
///
/// @details Converts VM slot arguments into the ABI expected by the runtime
///          library, allocates temporary buffers for return values, invokes the
///          descriptor's handler, and translates any power-trap metadata into VM
///          traps.
///
/// @param desc Runtime descriptor to invoke.
/// @param argBegin Pointer to the first argument slot (may be null when @p argCount is zero).
/// @param argCount Number of argument slots provided.
/// @param ctx Call context providing trap location metadata.
/// @return Slot containing the marshalled return value.
static VmResult executeDescriptor(const RuntimeDescriptor &desc,
                                  Slot *argBegin,
                                  std::size_t argCount,
                                  const RuntimeCallContext &ctx) {
    std::span<Slot> argSpan{};
    if (argBegin && argCount)
        argSpan = {argBegin, argCount};

    // Use stack-allocated marshalling buffer for small argument counts (HIGH-6)
    il::vm::PowStatus powStatus{};
    il::vm::MarshalledArgs marshalledArgs{};
    il::vm::marshalArgumentsInline(desc.signature, argSpan, powStatus, marshalledArgs);

    ResultBuffers buffers{};
    void *resultPtr = il::vm::resultBufferFor(desc.signature.retType.kind, buffers);
    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
    const int recoveryState = setjmp(recovery);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (recoveryState == 0) {
        desc.handler(marshalledArgs.empty() ? nullptr : marshalledArgs.data(), resultPtr);
        rt_trap_clear_recovery();
    } else {
        const TrapKind recoveredKind =
            il::vm::trapKindFromValue(static_cast<int32_t>(rt_trap_get_kind()));
        const int32_t recoveredCode = static_cast<int32_t>(rt_trap_get_code());
        const int32_t recoveredLine = static_cast<int32_t>(rt_trap_get_line());
        const char *recoveredMessage = rt_trap_get_error();
        std::string recoveredMessageCopy =
            (recoveredMessage && *recoveredMessage) ? recoveredMessage : "runtime trap";
        rt_trap_clear_recovery();
        SourceLoc recoveredLoc = ctx.loc;
        if (recoveredLine >= 0)
            recoveredLoc.line = static_cast<uint32_t>(recoveredLine);
        RuntimeBridge::trap(recoveredKind,
                            recoveredMessageCopy,
                            recoveredLoc,
                            ctx.function,
                            ctx.block,
                            recoveredCode);
        return Slot{};
    }

    std::span<const Slot> readonlyArgs{};
    if (argBegin && argCount)
        readonlyArgs = {argBegin, argCount};
    auto trap = il::vm::classifyPowTrap(desc, powStatus, readonlyArgs, buffers);
    if (trap.triggered) {
        // RuntimeBridge::trap escalates into vm_raise when a VM is active.
        RuntimeBridge::trap(trap.kind, trap.message, ctx.loc, ctx.function, ctx.block);
        return Slot{};
    }

    return il::vm::assignCallResult(desc.signature, buffers);
}

/// @brief Generic thunk that executes descriptors without VM-specific side effects.
///
/// @details The VM and frame parameters are unused for most runtime functions;
///          they are present to match the signature expected by the thunk table.
/// @param vm Active VM required by the uniform thunk signature.
/// @param frame Runtime frame metadata required by the uniform thunk signature.
/// @param ctx Call descriptor, arguments, and diagnostic context.
/// @return Marshalled runtime result slot.
static VmResult genericThunk(VM &vm, FrameInfo &frame, const RuntimeCallContext &ctx) {
    (void)vm;
    (void)frame;
    return executeDescriptor(*ctx.descriptor, ctx.argBegin, ctx.argCount, ctx);
}

/// @brief Construct the table of thunks indexed by runtime signature tags.
///
/// @details Each entry defaults to the generic thunk for now, but the table is
///          built as a constexpr helper so future specialised thunks can be
///          registered in one place.
constexpr std::array<Thunk, static_cast<std::size_t>(RtSig::Count)> buildThunkTable() {
    std::array<Thunk, static_cast<std::size_t>(RtSig::Count)> table{};
    table.fill(&genericThunk);
    return table;
}

/// @brief Access the lazily initialised thunk table.
///
/// @return Reference to the singleton thunk array used for runtime dispatch.
const std::array<Thunk, static_cast<std::size_t>(RtSig::Count)> &thunkTable() {
    static const auto table = buildThunkTable();
    return table;
}

/// @brief RAII helper that installs a runtime call context for the current thread.
struct ContextGuard {
    RuntimeCallContext *previous; ///< Context restored at scope exit.
    RuntimeCallContext *current; ///< Context installed by this guard.

    /// @brief Push the provided context as the thread-local active call.
    /// @param ctx Mutable call context installed for this scope.
    explicit ContextGuard(RuntimeCallContext &ctx) : previous(tlsContext), current(&ctx) {
        tlsContext = &ctx;
    }

    /// @brief Restore the previous context and clear transient diagnostic fields.
    ~ContextGuard() {
        if (current) {
            current->loc = {};
            current->function.clear();
            current->block.clear();
            current->message.clear();
            current->descriptor = nullptr;
            current->argBegin = nullptr;
            current->argCount = 0;
        }
        tlsContext = previous;
    }
};

using Operands = std::span<const Slot>;

/// @brief Aggregates information required to finalise a runtime trap.
struct TrapCtx {
    TrapKind kind; ///< Runtime trap classification.
    const std::string &message; ///< Borrowed human-readable diagnostic.
    const SourceLoc &loc; ///< Source location associated with the trap.
    const std::string &function; ///< Active function name.
    const std::string &block; ///< Active block label.
    VM *vm = nullptr; ///< Active VM, or null outside interpreted execution.
    VmError error{}; ///< Structured error delivered to VM/runtime consumers.
    FrameInfo frame{}; ///< Standalone frame metadata when no VM is active.
};

/// @brief Deliver a trap either to the active VM or to the call-site context.
///
/// @details When a VM is executing the trap escalates through @ref vm_raise.
///          Otherwise the trap information is recorded directly on the context
///          so higher layers can surface it to the user.
///
/// INVARIANT: If ctx.vm is non-null, VM::activeInstance() must also be non-null.
/// GUARANTEE: This function does not return to its caller when no handler catches.
/// @param ctx Fully populated trap context to intercept and deliver.
static void finalizeTrap(TrapCtx &ctx) {
    RuntimeBridge::interceptTrap(
        ctx.kind, ctx.error.code, ctx.message, ctx.loc, ctx.function, ctx.block);

    if (ctx.vm) {
        // Assert that activeInstance is consistent with ctx.vm
        ZANNA_TRAP_ASSERT(RuntimeBridge::hasActiveVm(),
                          "ActiveVMGuard inconsistency: ctx.vm set but no active VM");
        vm_raise_from_error(ctx.error);
        // Tests may override vm_trap() with a non-terminating observer. In that
        // case the trap has already been recorded on the active VM, and the
        // caller is intentionally choosing to continue after observing it.
        return;
    }

    std::string diagnostic = vm_format_error(ctx.error, ctx.frame);
    if (!ctx.message.empty()) {
        diagnostic += ": ";
        diagnostic += ctx.message;
    }
    rt_abort(diagnostic.c_str());
    // rt_abort does not return
}

/// @brief Populate overflow-specific diagnostics prior to finalising a trap.
///
/// @param ctx Aggregated trap context to populate.
/// @param opcode Opcode that triggered the overflow.
/// @param operands Operands involved in the failing operation.
static void handleOverflow(TrapCtx &ctx, Opcode opcode, const Operands &operands) {
    (void)opcode;
    (void)operands;
    finalizeTrap(ctx);
}

/// @brief Populate divide-by-zero diagnostics prior to finalising a trap.
///
/// @param ctx Aggregated trap context to populate.
/// @param opcode Opcode that triggered the trap.
/// @param operands Operands supplied to the operation.
static void handleDivByZero(TrapCtx &ctx, Opcode opcode, const Operands &operands) {
    (void)opcode;
    (void)operands;
    finalizeTrap(ctx);
}

/// @brief Finalise traps that do not require operand-specific formatting.
/// @param ctx Trap context forwarded unchanged to @ref finalizeTrap.
static void handleGenericTrap(TrapCtx &ctx) {
    finalizeTrap(ctx);
}

} // namespace

/// @brief Entry point invoked from the C runtime when a trap occurs.
/// @details Serves as the external hook that the C runtime calls when
/// `rt_abort`-style routines detect a fatal condition. The VM stores call-site
/// context in a thread-local pointer via `RuntimeBridge::call`; this hook relays
/// the trap through `RuntimeBridge::trap` so diagnostics carry function, block,
/// and source information.
#if defined(_WIN32)
// On Windows, vm_trap is provided by zanna_runtime.lib via alternatename.
// Tests can define their own vm_trap to override the default.
// We don't define vm_trap here to avoid duplicate symbol errors with lld-link.
#elif defined(__GNUC__) || defined(__clang__)
/// @brief Weak hook allowing embedders to override VM trap behaviour.
/// @param msg Null-terminated diagnostic, or @c nullptr for the default text.
extern "C" __attribute__((weak)) void vm_trap(const char *msg) {
    rt_abort(msg ? msg : "trap");
}
#else
/// @brief Default implementation that records traps on the active context.
/// @param msg Null-terminated diagnostic, or @c nullptr for the default text.
extern "C" void vm_trap(const char *msg) {
    rt_abort(msg ? msg : "trap");
}
#endif

namespace il::vm {

//===----------------------------------------------------------------------===//
// ExternRegistry Implementation
//===----------------------------------------------------------------------===//
//
// DESIGN NOTE: Extern Registry Scoping
// =====================================
//
// The extern registry supports two modes of operation:
//
// 1. PROCESS-GLOBAL REGISTRY (default):
//    A singleton registry protected by a mutex. All VM instances without a
//    per-VM registry share this global registry. Functions registered via
//    RuntimeBridge::registerExtern() go here.
//
// 2. PER-VM REGISTRY (opt-in):
//    Each VM can optionally hold a pointer to its own ExternRegistry. When
//    resolving extern calls via currentExternRegistry(), the active VM's
//    registry is checked first; if no match is found (or no per-VM registry
//    is configured), the process-global registry is consulted.
//
// Thread Safety:
// - The process-global registry is protected by an internal mutex.
// - Per-VM registries are NOT mutex-protected; they rely on the VM's single-
//   threaded execution model. Embedders must not modify a per-VM registry
//   from another thread while the VM is executing.
//
// Usage Pattern for Per-VM Registries:
//   auto reg = createExternRegistry();      // Create isolated registry
//   vm.setExternRegistry(reg.get());        // Assign to VM (non-owning)
//   registerExternIn(*reg, myExternDesc);   // Populate
//   // ... vm.run() ...
//   // reg must outlive vm
//
//===----------------------------------------------------------------------===//

namespace {

/// @brief Internal record for a registered external function.
struct ExtRecord {
    ExternDesc pub;                                ///< Public descriptor exposed to callers.
    il::runtime::RuntimeSignature runtimeSig;      ///< Converted runtime signature.
    il::runtime::RuntimeHandler handler = nullptr; ///< Native handler function.
};

} // namespace

/// @brief Concrete implementation of the ExternRegistry abstraction.
/// @details This struct holds the actual storage (map + mutex) for external
///          function registrations. It is intentionally defined in the .cpp
///          file to keep the header opaque.
struct ExternRegistry {
    std::mutex mutex;                                   ///< Protects concurrent access.
    std::unordered_map<std::string, ExtRecord> entries; ///< Name -> record mapping.
    bool strictMode = false; ///< When true, reject re-registration with different signature.
    std::atomic<uint32_t> refCount{1}; ///< Intrusive lifetime refs across VMs/workers.
};

namespace {

/// @brief Access the process-global extern registry singleton.
/// @return Reference to the lazily-initialized global registry.
ExternRegistry &globalRegistry() {
    static ExternRegistry instance;
    return instance;
}

/// @brief Compare two signatures for structural equality.
/// @details Two signatures are equal if they have the same parameter kinds
///          and return kinds in the same order. The name and attribute flags
///          (nothrow, readonly, pure) are ignored for this comparison.
/// @param a First signature.
/// @param b Second signature.
/// @return True if the signatures are structurally equivalent.
static bool signaturesEqual(const Signature &a, const Signature &b) {
    if (a.params.size() != b.params.size())
        return false;
    if (a.rets.size() != b.rets.size())
        return false;
    for (size_t i = 0; i < a.params.size(); ++i) {
        if (a.params[i].kind != b.params[i].kind)
            return false;
    }
    for (size_t i = 0; i < a.rets.size(); ++i) {
        if (a.rets[i].kind != b.rets[i].kind)
            return false;
    }
    return true;
}

/// @brief Convert a public external-signature kind to an IL runtime type.
/// @param k External ABI kind to translate.
/// @return Corresponding IL type; unknown values conservatively map to void.
static il::core::Type mapKind(il::runtime::signatures::SigParam::Kind k) {
    using K = il::runtime::signatures::SigParam::Kind;
    using il::core::Type;
    switch (k) {
        case K::I1:
            return Type(Type::Kind::I1);
        case K::I32:
            return Type(Type::Kind::I32);
        case K::I64:
            return Type(Type::Kind::I64);
        case K::F32:
            return Type(Type::Kind::F64);
        case K::F64:
            return Type(Type::Kind::F64);
        case K::Ptr:
            return Type(Type::Kind::Ptr);
        case K::Str:
            return Type(Type::Kind::Str);
    }
    return Type(Type::Kind::Void);
}

/// @brief Convert a public external signature to the runtime descriptor form.
/// @details Preserves parameter order, the first return kind, and behavioral
///          attributes while selecting no specialized trap class.
/// @param sig Public external signature to convert.
/// @return Runtime signature suitable for descriptor dispatch.
static il::runtime::RuntimeSignature toRuntimeSig(const Signature &sig) {
    il::runtime::RuntimeSignature rs;
    rs.paramTypes.reserve(sig.params.size());
    for (const auto &p : sig.params)
        rs.paramTypes.push_back(mapKind(p.kind));
    if (!sig.rets.empty())
        rs.retType = mapKind(sig.rets.front().kind);
    else
        rs.retType = il::core::Type(il::core::Type::Kind::Void);
    rs.trapClass = il::runtime::RuntimeTrapClass::None;
    rs.nothrow = sig.nothrow;
    rs.readonly = sig.readonly;
    rs.pure = sig.pure;
    return rs;
}
} // namespace

/// @brief Canonicalize an external name for case-insensitive registry lookup.
/// @param n External symbol name.
/// @return Lowercase owning key string.
std::string canonicalizeExternName(std::string_view n) {
    std::string out(n);
    for (auto &ch : out)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

static const RuntimeDescriptor *resolveRuntimeDescriptor(std::string_view name,
                                                         RuntimeDescriptor &localDesc);
static const RuntimeDescriptor *resolveExternDescriptor(std::string_view name,
                                                        RuntimeDescriptor &localDesc);
static Slot dispatchRuntimeCall(RuntimeCallContext &ctx,
                                std::string_view name,
                                const RuntimeDescriptor &desc,
                                VM *activeVm);

/// @brief Invoke a runtime helper identified by name on behalf of the VM.
///
/// @details Validates the callee descriptor, checks argument counts, installs
///          the call context for trap reporting, and dispatches through the thunk
///          table or directly when no VM is active.  On failure the function
///          records diagnostics and returns a zero-initialised slot.
///
/// @param ctx Mutable call context tracking diagnostics and temporary buffers.
/// @param name Runtime helper symbol to resolve.
/// @param args Argument slots supplied by the VM.
/// @param loc Source location associated with the call site.
/// @param fn Function name executing the call.
/// @param block Block label executing the call.
/// @return Slot containing the runtime result or zero on trap.
Slot RuntimeBridge::call(RuntimeCallContext &ctx,
                         std::string_view name,
                         std::span<const Slot> args,
                         const SourceLoc &loc,
                         const std::string &fn,
                         const std::string &block) {
    std::vector<Slot> mutableArgs(args.begin(), args.end());
    return RuntimeBridge::callMutable(
        ctx, name, std::span<Slot>{mutableArgs.data(), mutableArgs.size()}, loc, fn, block);
}

/// @brief Invoke a runtime helper by name with mutable argument slots.
/// @details Installs trap context, validates arity, and dispatches through the
///          active extern or built-in descriptor. Mutations made by the native
///          handler to argument slots remain visible to the opcode handler.
/// @param ctx Mutable call context populated for diagnostics and dispatch.
/// @param name External or built-in runtime helper name.
/// @param args Mutable argument slots exposed to the native handler.
/// @param loc Source location of the call.
/// @param fn Calling function name.
/// @param block Calling block label.
/// @return Runtime result slot, or a zero-initialized slot after failure.
Slot RuntimeBridge::callMutable(RuntimeCallContext &ctx,
                                std::string_view name,
                                std::span<Slot> args,
                                const SourceLoc &loc,
                                const std::string &fn,
                                const std::string &block) {
    ctx.loc = loc;
    ctx.function = fn;
    ctx.block = block;
    ContextGuard guard(ctx);
    Slot result{};

    // Resolve against per-VM externs first, then process-global extern overrides,
    // then built-in runtime descriptors.
    il::runtime::RuntimeDescriptor localDesc;
    const il::runtime::RuntimeDescriptor *desc = resolveRuntimeDescriptor(name, localDesc);
    if (!desc) {
        RuntimeBridge::trap(
            TrapKind::DomainError, diag::formatUnknownRuntimeHelper(name), loc, fn, block);
        return result;
    }
    if (!validateArgumentCount(*desc, args, loc, fn, block))
        return result;

    ctx.descriptor = desc;
    ctx.argBegin = args.empty() ? nullptr : args.data();
    ctx.argCount = args.size();

    VM *activeVm = VM::activeInstance();
    result = dispatchRuntimeCall(ctx, name, *desc, activeVm);
    rt_gc_safepoint();

    return result;
}

/// @brief Invoke an already resolved runtime descriptor with copied arguments.
/// @details Copies the read-only argument span so mutable ABI handlers cannot
///          alter caller-owned slots, then delegates to @ref callMutable.
/// @param ctx Mutable call context populated for diagnostics and dispatch.
/// @param desc Built-in runtime descriptor to invoke or override by name.
/// @param args Read-only VM argument slots.
/// @param loc Source location of the call.
/// @param fn Calling function name.
/// @param block Calling block label.
/// @return Runtime result slot, or a zero-initialized slot after failure.
Slot RuntimeBridge::call(RuntimeCallContext &ctx,
                         const il::runtime::RuntimeDescriptor &desc,
                         std::span<const Slot> args,
                         const SourceLoc &loc,
                         const std::string &fn,
                         const std::string &block) {
    std::vector<Slot> mutableArgs(args.begin(), args.end());
    return RuntimeBridge::callMutable(
        ctx, desc, std::span<Slot>{mutableArgs.data(), mutableArgs.size()}, loc, fn, block);
}

/// @brief Invoke a resolved runtime descriptor with mutable argument slots.
/// @details Applies extern overrides for the descriptor name, validates the
///          effective signature, and preserves slot mutations for VM copy-back
///          processing.
/// @param ctx Mutable call context populated for diagnostics and dispatch.
/// @param desc Built-in runtime descriptor to invoke or override by name.
/// @param args Mutable argument slots exposed to the native handler.
/// @param loc Source location of the call.
/// @param fn Calling function name.
/// @param block Calling block label.
/// @return Runtime result slot, or a zero-initialized slot after failure.
Slot RuntimeBridge::callMutable(RuntimeCallContext &ctx,
                                const il::runtime::RuntimeDescriptor &desc,
                                std::span<Slot> args,
                                const SourceLoc &loc,
                                const std::string &fn,
                                const std::string &block) {
    ctx.loc = loc;
    ctx.function = fn;
    ctx.block = block;
    ContextGuard guard(ctx);
    Slot result{};

    il::runtime::RuntimeDescriptor localDesc;
    const il::runtime::RuntimeDescriptor *effectiveDesc =
        resolveExternDescriptor(desc.name, localDesc);
    if (!effectiveDesc)
        effectiveDesc = &desc;

    if (!validateArgumentCount(*effectiveDesc, args, loc, fn, block))
        return result;

    ctx.descriptor = effectiveDesc;
    ctx.argBegin = args.empty() ? nullptr : args.data();
    ctx.argCount = args.size();

    VM *activeVm = VM::activeInstance();
    result = dispatchRuntimeCall(ctx, effectiveDesc->name, *effectiveDesc, activeVm);
    rt_gc_safepoint();

    return result;
}

/// @brief Resolve an external override into a temporary runtime descriptor.
/// @details Checks the active VM registry before the process-global registry.
/// @param name External name to resolve case-insensitively.
/// @param [out] localDesc Storage populated when an external is found.
/// @return Pointer to @p localDesc on success, or @c nullptr when absent.
static const RuntimeDescriptor *resolveExternDescriptor(std::string_view name,
                                                        RuntimeDescriptor &localDesc) {
    il::runtime::RuntimeSignature sig;
    il::runtime::RuntimeHandler handler = nullptr;
    const ExternDesc *extDesc = nullptr;

    if (ExternRegistry *activeRegistry = RuntimeBridge::activeVmRegistry()) {
        extDesc = il::vm::resolveExternIn(*activeRegistry, name, &sig, &handler);
    }

    if (!extDesc) {
        extDesc = il::vm::resolveExternIn(processGlobalExternRegistry(), name, &sig, &handler);
    }

    if (!extDesc)
        return nullptr;

    localDesc.name = extDesc->name;
    localDesc.signature = sig;
    localDesc.handler = handler;
    localDesc.lowering = {};
    return &localDesc;
}

/// @brief Resolve a runtime call through extern overrides then built-ins.
/// @param name Runtime helper name.
/// @param [out] localDesc Storage used when an external override is selected.
/// @return Effective descriptor, or @c nullptr when no helper is registered.
static const RuntimeDescriptor *resolveRuntimeDescriptor(std::string_view name,
                                                         RuntimeDescriptor &localDesc) {
    if (const RuntimeDescriptor *ext = resolveExternDescriptor(name, localDesc))
        return ext;

    return il::runtime::findRuntimeDescriptor(name);
}

/// @brief Dispatch a validated runtime call inside or outside an active VM.
/// @details VM calls select a signature thunk when available; standalone calls
///          execute the descriptor directly.
/// @param ctx Prepared call context containing arguments and descriptor.
/// @param name Runtime name used for thunk lookup.
/// @param desc Effective descriptor.
/// @param activeVm Active VM pointer, or @c nullptr for standalone dispatch.
/// @return Marshalled result slot.
static Slot dispatchRuntimeCall(RuntimeCallContext &ctx,
                                std::string_view name,
                                const RuntimeDescriptor &desc,
                                VM *activeVm) {
    if (activeVm) {
        FrameInfo frame{};
        std::optional<RtSig> sigId = il::runtime::findRuntimeSignatureId(name);
        Thunk thunk = nullptr;
        if (sigId && static_cast<std::size_t>(*sigId) < thunkTable().size())
            thunk = thunkTable()[static_cast<std::size_t>(*sigId)];
        if (!thunk)
            thunk = &genericThunk;
        return thunk(*activeVm, frame, ctx);
    }

    return executeDescriptor(desc, ctx.argBegin, ctx.argCount, ctx);
}

/// @brief Record a runtime trap and escalate it to the VM when applicable.
///
/// @details Populates a @ref TrapCtx structure with diagnostic metadata and
///          delegates to specialised helpers based on @p kind before finalising
///          delivery via @ref finalizeTrap.
///
/// @param kind Kind of trap raised by the runtime.
/// @param msg Human-readable diagnostic payload.
/// @param loc Source location associated with the trap.
/// @param fn Function name active when the trap occurred.
/// @param block Block label active when the trap occurred.
/// @param code Runtime-specific numeric error code.
void RuntimeBridge::trap(TrapKind kind,
                         const std::string &msg,
                         const SourceLoc &loc,
                         const std::string &fn,
                         const std::string &block,
                         int32_t code) {
    TrapCtx ctx{kind, msg, loc, fn, block};
    ctx.vm = VM::activeInstance();
    ctx.error.kind = kind;
    ctx.error.code = code;
    ctx.error.ip = 0;
    ctx.error.line = loc.hasLine() ? static_cast<int32_t>(loc.line) : -1;
    if (ctx.vm) {
        /// @brief Publish trap source and function context into an active VM.
        /// @param vm Active VM receiving context updates.
        /// @param trapLoc Trap source location.
        /// @param fnName Active function name.
        /// @param blockLabel Active block label.
        auto populateVm = [](VM &vm,
                             const SourceLoc &trapLoc,
                             const std::string &fnName,
                             const std::string &blockLabel) {
            if (trapLoc.hasFile()) {
                vm.currentContext.loc = trapLoc;
                vm.runtimeContext.loc = trapLoc;
            } else {
                vm.runtimeContext.loc = {};
            }
            if (!fnName.empty()) {
                vm.runtimeContext.function = fnName;
            } else {
                vm.runtimeContext.function.clear();
                vm.lastTrap.frame.function.clear();
            }
            if (!blockLabel.empty()) {
                vm.runtimeContext.block = blockLabel;
            } else {
                vm.runtimeContext.block.clear();
            }
            if (!trapLoc.hasLine())
                vm.lastTrap.frame.line = -1;
        };
        populateVm(*ctx.vm, loc, fn, block);
        ctx.vm->runtimeContext.message = msg;
    } else {
        /// @brief Populate fallback trap records when no VM instance is active.
        /// @param c Trap context receiving error and frame metadata.
        /// @param trapLoc Trap source location.
        /// @param fnName Active function name, or empty when unavailable.
        auto populateNoVm = [](TrapCtx &c, const SourceLoc &trapLoc, const std::string &fnName) {
            c.error.ip = 0;
            c.error.line = trapLoc.hasLine() ? static_cast<int32_t>(trapLoc.line) : -1;

            c.frame.function = fnName.empty() ? std::string("<unknown>") : fnName;
            c.frame.ip = 0;
            c.frame.line = c.error.line;
            c.frame.handlerInstalled = false;
        };
        populateNoVm(ctx, loc, fn);
    }

    constexpr Opcode trapOpcode = Opcode::Trap;
    const Operands noOperands{};

    switch (kind) {
        case TrapKind::Overflow:
            handleOverflow(ctx, trapOpcode, noOperands);
            break;
        case TrapKind::DivideByZero:
            handleDivByZero(ctx, trapOpcode, noOperands);
            break;
        default:
            handleGenericTrap(ctx);
            break;
    }
    // Tests may override vm_trap() with a non-terminating observer. In that
    // configuration finalizeTrap() intentionally returns after recording the
    // trap, so the bridge must not assume control flow is impossible here.
    return;
}

/// @brief Notify the installed trap interceptor and unwind with a signal.
/// @details Returns immediately when no interceptor is installed; otherwise the
///          callback observes the complete signal before that signal is thrown.
/// @param kind Runtime trap classification.
/// @param code Runtime-specific numeric error code.
/// @param msg Human-readable diagnostic.
/// @param loc Source location associated with the trap.
/// @param fn Active function name.
/// @param block Active block label.
void RuntimeBridge::interceptTrap(TrapKind kind,
                                  int32_t code,
                                  const std::string &msg,
                                  const SourceLoc &loc,
                                  const std::string &fn,
                                  const std::string &block) {
    if (!tlsTrapInterceptor)
        return;

    RuntimeTrapSignal signal{kind, code, msg, loc, fn, block};
    tlsTrapInterceptor(signal, tlsTrapInterceptorUserData);
    throw signal;
}

/// @brief Retrieve the currently installed runtime call context, if any.
///
/// @return Pointer to the context managed by @ref ContextGuard, or @c nullptr when inactive.
const RuntimeCallContext *RuntimeBridge::activeContext() {
    return tlsContext;
}

/// @brief Determine whether this thread is executing inside a VM.
/// @return @c true when @ref VM::activeInstance is non-null.
bool RuntimeBridge::hasActiveVm() {
    return VM::activeInstance() != nullptr;
}

/// @brief Retrieve the external registry attached to the active VM.
/// @return Non-owning registry pointer, or @c nullptr when no VM or per-VM
///         registry is active.
ExternRegistry *RuntimeBridge::activeVmRegistry() {
    if (VM *vm = VM::activeInstance())
        return vm->externRegistry();
    return nullptr;
}

/// @brief Vector convenience overload for named runtime dispatch.
/// @param ctx Mutable call context populated for diagnostics and dispatch.
/// @param name Runtime helper name.
/// @param args Read-only argument vector.
/// @param loc Source location of the call.
/// @param fn Calling function name.
/// @param block Calling block label.
/// @return Runtime result slot.
Slot RuntimeBridge::call(RuntimeCallContext &ctx,
                         std::string_view name,
                         const std::vector<Slot> &args,
                         const SourceLoc &loc,
                         const std::string &fn,
                         const std::string &block) {
    return RuntimeBridge::call(
        ctx, name, std::span<const Slot>{args.data(), args.size()}, loc, fn, block);
}

/// @brief Initializer-list convenience overload for named runtime dispatch.
/// @param ctx Mutable call context populated for diagnostics and dispatch.
/// @param name Runtime helper name.
/// @param args Temporary argument list.
/// @param loc Source location of the call.
/// @param fn Calling function name.
/// @param block Calling block label.
/// @return Runtime result slot.
Slot RuntimeBridge::call(RuntimeCallContext &ctx,
                         std::string_view name,
                         std::initializer_list<Slot> args,
                         const SourceLoc &loc,
                         const std::string &fn,
                         const std::string &block) {
    return RuntimeBridge::call(
        ctx, name, std::span<const Slot>{args.begin(), args.size()}, loc, fn, block);
}

//===----------------------------------------------------------------------===//
// ExternRegistry Free Functions
//===----------------------------------------------------------------------===//

/// @brief Access the process-global external registry.
/// @return Reference to the lazily initialized registry singleton.
ExternRegistry &processGlobalExternRegistry() {
    return globalRegistry();
}

/// @brief Select the active VM registry or the process-global fallback.
/// @return Registry used for external operations on the current thread.
ExternRegistry &currentExternRegistry() {
    // Check for active VM with a per-VM registry configured.
    // Falls back to the process-global registry when:
    // - No VM is currently active, or
    // - The active VM has no per-VM registry assigned.
    if (ExternRegistry *reg = RuntimeBridge::activeVmRegistry())
        return *reg;
    return globalRegistry();
}

/// @brief Add an intrusive lifetime reference to a registry.
/// @param registry Registry to retain; @c nullptr is ignored.
void retainExternRegistry(ExternRegistry *registry) {
    if (!registry)
        return;
    registry->refCount.fetch_add(1, std::memory_order_relaxed);
}

/// @brief Release an intrusive registry reference and destroy at zero.
/// @param registry Registry to release; @c nullptr is ignored.
void releaseExternRegistry(ExternRegistry *registry) noexcept {
    if (!registry)
        return;
    if (registry->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete registry;
}

/// @brief Add or replace an external within a specific registry.
/// @details Re-registration with a structurally different signature is rejected
///          only in strict mode; compatible or non-strict replacements update
///          the stored descriptor and runtime form.
/// @param registry Target registry.
/// @param ext Public external descriptor to store.
/// @return Success, or @ref ExternRegisterResult::SignatureMismatch in strict
///         mode for an incompatible replacement.
ExternRegisterResult registerExternIn(ExternRegistry &registry, const ExternDesc &ext) {
    ExtRecord rec;
    rec.pub = ext;
    rec.runtimeSig = toRuntimeSig(ext.signature);
    rec.handler = reinterpret_cast<il::runtime::RuntimeHandler>(ext.fn);
    const std::string key = canonicalizeExternName(ext.name);
    std::lock_guard<std::mutex> lock(registry.mutex);

    // Check for existing entry with same name
    auto it = registry.entries.find(key);
    if (it != registry.entries.end()) {
        // Already registered - check signature compatibility
        if (signaturesEqual(it->second.pub.signature, ext.signature)) {
            // Same signature: update silently (no-op if fn is also the same)
            it->second = std::move(rec);
            return ExternRegisterResult::Success;
        } else {
            // Different signature: error in strict mode, warning otherwise
            if (registry.strictMode) {
                return ExternRegisterResult::SignatureMismatch;
            }
            // Non-strict mode: overwrite and continue
            it->second = std::move(rec);
            return ExternRegisterResult::Success;
        }
    }

    // New registration
    registry.entries.emplace(key, std::move(rec));
    return ExternRegisterResult::Success;
}

/// @brief Remove an external from a specific registry.
/// @param registry Registry to modify.
/// @param name Case-insensitive external name.
/// @return @c true when an entry was erased.
bool unregisterExternIn(ExternRegistry &registry, std::string_view name) {
    const std::string key = canonicalizeExternName(name);
    std::lock_guard<std::mutex> lock(registry.mutex);
    return registry.entries.erase(key) > 0;
}

/// @brief Find an external and copy its public descriptor to thread-local storage.
/// @details The returned pointer remains valid until its rotating thread-local
///          slot is reused by later lookups on the same thread.
/// @param registry Registry to search.
/// @param name Case-insensitive external name.
/// @return Pointer to a thread-local descriptor copy, or @c nullptr if absent.
const ExternDesc *findExternIn(ExternRegistry &registry, std::string_view name) {
    const std::string key = canonicalizeExternName(name);
    thread_local std::array<ExternDesc, 8> tlsExternCopies{};
    thread_local size_t tlsExternCopyIndex = 0;
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.entries.find(key);
    if (it == registry.entries.end())
        return nullptr;
    ExternDesc &slot = tlsExternCopies[tlsExternCopyIndex];
    tlsExternCopyIndex = (tlsExternCopyIndex + 1) % tlsExternCopies.size();
    slot = it->second.pub;
    return &slot;
}

/// @brief Resolve public and runtime forms of an external atomically.
/// @param registry Registry to search.
/// @param name Case-insensitive external name.
/// @param [out] outSig Optional destination for the converted runtime signature.
/// @param [out] outHandler Optional destination for the native handler.
/// @return Pointer to a thread-local public descriptor copy, or @c nullptr if
///         absent.
const ExternDesc *resolveExternIn(ExternRegistry &registry,
                                  std::string_view name,
                                  il::runtime::RuntimeSignature *outSig,
                                  il::runtime::RuntimeHandler *outHandler) {
    const std::string key = canonicalizeExternName(name);
    thread_local std::array<ExternDesc, 8> tlsExternCopies{};
    thread_local size_t tlsExternCopyIndex = 0;
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto it = registry.entries.find(key);
    if (it == registry.entries.end())
        return nullptr;
    if (outSig)
        *outSig = it->second.runtimeSig;
    if (outHandler)
        *outHandler = it->second.handler;
    ExternDesc &slot = tlsExternCopies[tlsExternCopyIndex];
    tlsExternCopyIndex = (tlsExternCopyIndex + 1) % tlsExternCopies.size();
    slot = it->second.pub;
    return &slot;
}

//===----------------------------------------------------------------------===//
// ExternRegistry Strict Mode API
//===----------------------------------------------------------------------===//

/// @brief Enable or disable incompatible-replacement rejection.
/// @param registry Registry whose policy is changed.
/// @param enabled Whether strict signature matching is required.
void setExternRegistryStrictMode(ExternRegistry &registry, bool enabled) {
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.strictMode = enabled;
}

/// @brief Query a registry's signature replacement policy.
/// @param registry Registry whose policy is queried.
/// @return @c true when incompatible re-registration is rejected.
bool isExternRegistryStrictMode(const ExternRegistry &registry) {
    // Note: reading a bool is atomic on all supported platforms, but we lock
    // for consistency with the setter and to be future-proof.
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(registry.mutex));
    return registry.strictMode;
}

//===----------------------------------------------------------------------===//
// ExternRegistry Factory and Deleter
//===----------------------------------------------------------------------===//

/// @brief Release the intrusive reference owned by a registry smart pointer.
/// @param reg Registry pointer to release.
void ExternRegistryDeleter::operator()(ExternRegistry *reg) const noexcept {
    releaseExternRegistry(reg);
}

/// @brief Allocate an isolated external registry.
/// @return Owning smart pointer whose deleter releases the initial intrusive
///         reference.
ExternRegistryPtr createExternRegistry() {
    return ExternRegistryPtr(new ExternRegistry());
}

/// @brief Install a thread-local runtime trap interceptor for this scope.
/// @details Saves the previously installed callback and user data so nested
///          interceptors compose correctly.
/// @param interceptor Callback invoked before trap escalation.
/// @param userData Opaque pointer forwarded to @p interceptor.
ScopedRuntimeTrapInterceptor::ScopedRuntimeTrapInterceptor(RuntimeTrapInterceptor interceptor,
                                                           void *userData)
    : previousInterceptor_(tlsTrapInterceptor), previousUserData_(tlsTrapInterceptorUserData) {
    tlsTrapInterceptor = interceptor;
    tlsTrapInterceptorUserData = userData;
}

/// @brief Restore the interceptor that was active before construction.
ScopedRuntimeTrapInterceptor::~ScopedRuntimeTrapInterceptor() {
    tlsTrapInterceptor = previousInterceptor_;
    tlsTrapInterceptorUserData = previousUserData_;
}

//===----------------------------------------------------------------------===//
// RuntimeBridge Static Methods (Delegate to Process-Global Registry)
//===----------------------------------------------------------------------===//

/// @brief Register or replace an external in the process-global registry.
/// @param ext Public descriptor to register.
void RuntimeBridge::registerExtern(const ExternDesc &ext) {
    registerExternIn(processGlobalExternRegistry(), ext);
}

/// @brief Remove a process-global external.
/// @param name Case-insensitive external name.
/// @return @c true when an entry was removed.
bool RuntimeBridge::unregisterExtern(std::string_view name) {
    return unregisterExternIn(processGlobalExternRegistry(), name);
}

/// @brief Find a process-global external.
/// @param name Case-insensitive external name.
/// @return Pointer to a thread-local descriptor copy, or @c nullptr if absent.
const ExternDesc *RuntimeBridge::findExtern(std::string_view name) {
    return findExternIn(processGlobalExternRegistry(), name);
}

} // namespace il::vm
