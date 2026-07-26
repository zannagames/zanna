//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Adapter between VM execution and the C runtime library.
 *
 * Declares the bridge used to invoke runtime helpers, manage trap diagnostics,
 * and register external functions callable from IL. The bridge supports both a
 * process-global extern registry and optional per-VM registries.
 *
 * @section registry Extern Registry Design
 * The extern registry maps external function names to their descriptors and
 * native implementations, enabling IL code to call host-provided functions.
 *
 * @par Resolution order
 * - The active VM's per-VM registry is consulted first when present
 * - The process-global registry is consulted next
 * - Built-in runtime descriptors are used only if no extern override matches
 *
 * @par Current implementation
 * - All process-global registrations are stored in a singleton registry
 * - A VM may also point at its own `ExternRegistry`
 * - Registration and lookup are mutex-protected per registry
 * - Functions registered via `registerExtern()` are visible process-wide unless
 *   a VM-specific registration with the same name overrides them
 *
 * This design supports both shared host integrations and per-VM embedding
 * scenarios without changing the extern call surface seen by IL code.
 */
//===----------------------------------------------------------------------===//

#pragma once

#include "rt.hpp"
#include "support/source_location.hpp"
#include "zanna/vm/RuntimeBridge.hpp"
#include "vm/Trap.hpp"

#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace il::runtime {
struct RuntimeDescriptor;
struct RuntimeSignature;
/// @brief Generic runtime-call handler signature used by VM marshalling.
/// @param args Array of pointers to argument storage.
/// @param result Pointer to result storage, or null for unit results.
using RuntimeHandler = void (*)(void **args, void *result);
} // namespace il::runtime

namespace il::vm {

union Slot; // defined in VM.hpp

//===----------------------------------------------------------------------===//
// ExternRegistry - Abstraction for external function registration
//===----------------------------------------------------------------------===//

/**
 * @brief Opaque handle to an extern registry.
 *
 * This struct wraps the underlying storage for external function registrations.
 * Registries may be used process-wide or attached to individual VM instances.
 * The implementation stays opaque so registry storage, locking, and lifetime
 * management can evolve without changing the public API surface.
 */
struct ExternRegistry;

/// @brief Access the process-global extern registry singleton.
/// @details This registry is shared by all VM instances in the process.
///          It is lazily initialized on first access and persists for the
///          lifetime of the process.
/// @return Reference to the process-global extern registry.
/// @note Thread-safe: the registry uses internal synchronization.
ExternRegistry &processGlobalExternRegistry();

/// @brief Access the extern registry for the current context.
/// @details Returns the active VM's per-VM registry when one is attached,
///          otherwise falls back to the process-global registry.
/// @return Reference to the appropriate extern registry.
ExternRegistry &currentExternRegistry();

/// @brief Increment the lifetime reference count for @p registry.
/// @details Registries are shared across VMs and worker payloads. Callers that
///          store a raw registry pointer beyond the lifetime of an owning
///          ExternRegistryPtr must retain it explicitly.
/// @param registry Registry to retain; @c nullptr is ignored.
void retainExternRegistry(ExternRegistry *registry);

/// @brief Release a previously retained extern registry reference.
/// @details Deletes the registry when the final retained reference is released.
/// @param registry Registry to release; @c nullptr is ignored.
void releaseExternRegistry(ExternRegistry *registry) noexcept;

/// @brief Register an external function in the specified registry.
/// @param registry Target registry (use `processGlobalExternRegistry()` for global).
/// @param ext Descriptor of the external function to register.
/// @return Success on successful registration. SignatureMismatch if strict mode
///         is enabled and a function with the same name but different signature
///         is already registered.
/// @note In non-strict mode (default), this always succeeds and overwrites any
///       existing registration with the same name (case-insensitive).
ExternRegisterResult registerExternIn(ExternRegistry &registry, const ExternDesc &ext);

/// @brief Unregister an external function from the specified registry.
/// @param registry Target registry.
/// @param name Name of the external function to unregister (case-insensitive).
/// @return @c true if a function was unregistered; @c false if not found.
bool unregisterExternIn(ExternRegistry &registry, std::string_view name);

/// @brief Find an external function descriptor in the specified registry.
/// @param registry Target registry.
/// @param name Name of the external function to find (case-insensitive).
/// @return Pointer to the descriptor if found; nullptr otherwise.
const ExternDesc *findExternIn(ExternRegistry &registry, std::string_view name);

/// @brief Resolve an external function for invocation.
/// @param registry Target registry.
/// @param name Name of the external function (case-insensitive).
/// @param [out] outSig Receives the runtime signature if found.
/// @param [out] outHandler Receives the native handler if found.
/// @return Pointer to the public descriptor if found; nullptr otherwise.
const ExternDesc *resolveExternIn(ExternRegistry &registry,
                                  std::string_view name,
                                  il::runtime::RuntimeSignature *outSig,
                                  il::runtime::RuntimeHandler *outHandler);

//===----------------------------------------------------------------------===//
// RuntimeCallContext
//===----------------------------------------------------------------------===//

/// @brief Stores runtime call metadata for trap diagnostics.
struct RuntimeCallContext {
    il::support::SourceLoc loc{}; ///< Source location of the active runtime call.
    std::string function;         ///< Name of the calling function.
    std::string block;            ///< Label of the calling basic block.
    std::string message;          ///< Supplemental diagnostic message from runtime.
    const il::runtime::RuntimeDescriptor *descriptor =
        nullptr;              ///< Descriptor of active runtime helper.
    Slot *argBegin = nullptr; ///< Pointer to first argument slot for the active call.
    std::size_t argCount = 0; ///< Number of argument slots.
};

/// @brief Exception used to route runtime traps back into alternate executors.
/// @details Standard VM execution routes traps through @ref vm_raise. Bytecode
///          execution can temporarily install a thread-local interceptor so
///          runtime faults become BytecodeVM traps instead of aborting.
struct RuntimeTrapSignal : std::exception {
    TrapKind kind{}; ///< Runtime trap classification.
    int32_t code = 0; ///< Runtime-specific numeric error code.
    std::string message; ///< Owning human-readable diagnostic.
    il::support::SourceLoc loc{}; ///< Source location associated with the trap.
    std::string function; ///< Function active when the trap occurred.
    std::string block; ///< Block active when the trap occurred.

    /// @brief Construct a complete trap signal for interception.
    /// @param trapKind Runtime trap classification.
    /// @param trapCode Runtime-specific numeric error code.
    /// @param trapMessage Human-readable diagnostic.
    /// @param trapLoc Source location associated with the trap.
    /// @param trapFunction Active function name.
    /// @param trapBlock Active block label.
    RuntimeTrapSignal(TrapKind trapKind,
                      int32_t trapCode,
                      std::string trapMessage,
                      il::support::SourceLoc trapLoc,
                      std::string trapFunction,
                      std::string trapBlock)
        : kind(trapKind), code(trapCode), message(std::move(trapMessage)), loc(trapLoc),
          function(std::move(trapFunction)), block(std::move(trapBlock)) {}

    /// @brief Expose the diagnostic through @c std::exception.
    /// @return Null-terminated pointer into @ref message.
    const char *what() const noexcept override {
        return message.c_str();
    }
};

/// @brief Callback invoked before a runtime trap is rethrown to an alternate executor.
/// @param signal Immutable trap signal containing diagnostics and source context.
/// @param userData Opaque pointer supplied when the interceptor was installed.
using RuntimeTrapInterceptor = void (*)(const RuntimeTrapSignal &signal, void *userData);

/// @brief RAII helper that installs a thread-local runtime trap interceptor.
class ScopedRuntimeTrapInterceptor {
  public:
    /// @brief Install an interceptor and opaque user data for this thread.
    /// @param interceptor Callback invoked for intercepted traps.
    /// @param userData Opaque pointer forwarded to @p interceptor.
    ScopedRuntimeTrapInterceptor(RuntimeTrapInterceptor interceptor, void *userData);
    /// @brief Restore the interceptor that was active before construction.
    ~ScopedRuntimeTrapInterceptor();

    /// @brief Interceptor scopes cannot be copied.
    ScopedRuntimeTrapInterceptor(const ScopedRuntimeTrapInterceptor &) = delete;
    /// @brief Interceptor scopes cannot be copy-assigned.
    ScopedRuntimeTrapInterceptor &operator=(const ScopedRuntimeTrapInterceptor &) = delete;

  private:
    RuntimeTrapInterceptor previousInterceptor_ = nullptr; ///< Callback to restore.
    void *previousUserData_ = nullptr; ///< Opaque data to restore.
};

/**
 * @brief Bridge between VM execution and native runtime functions.
 *
 * Provides a registry for external functions and manages the calling
 * convention between IL code and native C/C++ implementations.
 * All methods are static as this serves as a global registry.
 *
 * ## Extern Registry
 *
 * The extern registry methods (`registerExtern`, `unregisterExtern`, `findExtern`)
 * operate on the **process-global** extern registry. All VM instances in the
 * process share this registry. For explicit registry control, use the free
 * functions `registerExternIn()`, `unregisterExternIn()`, and `findExternIn()`
 * with `processGlobalExternRegistry()` or a future per-VM registry.
 *
 * @invariant External functions must be registered before VM execution begins.
 * @invariant Runtime calls maintain a thread-local context stack.
 *
 * @see ExternRegistry for the underlying abstraction.
 * @see processGlobalExternRegistry() for explicit global registry access.
 */
class RuntimeBridge {
  public:
    /// @brief Invoke runtime function @p name with arguments @p args.
    /// @param ctx Runtime call context receiving descriptor and trap metadata.
    /// @param name Runtime function symbol.
    /// @param args Evaluated argument slots.
    /// @param loc Source location of call instruction.
    /// @param fn Calling function name.
    /// @param block Calling block label.
    /// @return Result slot from runtime call.
    static Slot call(RuntimeCallContext &ctx,
                     std::string_view name,
                     std::span<const Slot> args,
                     const il::support::SourceLoc &loc,
                     const std::string &fn,
                     const std::string &block);

    /// @brief Invoke runtime function @p name with mutable argument slots.
    /// @details VM opcode handlers use this overload so runtime helpers that
    ///          mutate by-reference argument slots can be copied back to the
    ///          originating registers or stack locations after the call.
    /// @param ctx Runtime call context receiving trap metadata.
    /// @param name Runtime function symbol.
    /// @param args Mutable evaluated argument slots.
    /// @param loc Source location of call instruction.
    /// @param fn Calling function name.
    /// @param block Calling block label.
    /// @return Result slot from runtime call.
    static Slot callMutable(RuntimeCallContext &ctx,
                            std::string_view name,
                            std::span<Slot> args,
                            const il::support::SourceLoc &loc,
                            const std::string &fn,
                            const std::string &block);

    /// @brief Invoke an already resolved built-in runtime descriptor.
    /// @details Keeps RuntimeBridge validation and trap context handling while
    ///          avoiding repeated descriptor lookup on hot bytecode calls.
    /// @param ctx Runtime call context receiving descriptor and trap metadata.
    /// @param desc Resolved built-in descriptor, subject to external override.
    /// @param args Evaluated argument slots copied before native dispatch.
    /// @param loc Source location of call instruction.
    /// @param fn Calling function name.
    /// @param block Calling block label.
    /// @return Result slot from the effective runtime call.
    static Slot call(RuntimeCallContext &ctx,
                     const il::runtime::RuntimeDescriptor &desc,
                     std::span<const Slot> args,
                     const il::support::SourceLoc &loc,
                     const std::string &fn,
                     const std::string &block);

    /// @brief Invoke a resolved runtime descriptor with mutable argument slots.
    /// @details Mirrors @ref callMutable for name-based dispatch while allowing
    ///          callers that already have a descriptor to preserve slot mutation
    ///          semantics without unsafe const casts.
    /// @param ctx Runtime call context receiving descriptor and trap metadata.
    /// @param desc Resolved built-in descriptor, subject to external override.
    /// @param args Mutable evaluated argument slots.
    /// @param loc Source location of call instruction.
    /// @param fn Calling function name.
    /// @param block Calling block label.
    /// @return Result slot from the effective runtime call.
    static Slot callMutable(RuntimeCallContext &ctx,
                            const il::runtime::RuntimeDescriptor &desc,
                            std::span<Slot> args,
                            const il::support::SourceLoc &loc,
                            const std::string &fn,
                            const std::string &block);

    /// @brief Backward-compatible vector overload for named runtime calls.
    /// @param ctx Runtime call context receiving descriptor and trap metadata.
    /// @param name Runtime function symbol.
    /// @param args Evaluated argument vector.
    /// @param loc Source location of call instruction.
    /// @param fn Calling function name.
    /// @param block Calling block label.
    /// @return Result slot from the runtime call.
    static Slot call(RuntimeCallContext &ctx,
                     std::string_view name,
                     const std::vector<Slot> &args,
                     const il::support::SourceLoc &loc,
                     const std::string &fn,
                     const std::string &block);

    /// @brief Convenience initializer-list overload for named runtime calls.
    /// @param ctx Runtime call context receiving descriptor and trap metadata.
    /// @param name Runtime function symbol.
    /// @param args Temporary list of evaluated argument slots.
    /// @param loc Source location of call instruction.
    /// @param fn Calling function name.
    /// @param block Calling block label.
    /// @return Result slot from the runtime call.
    static Slot call(RuntimeCallContext &ctx,
                     std::string_view name,
                     std::initializer_list<Slot> args,
                     const il::support::SourceLoc &loc,
                     const std::string &fn,
                     const std::string &block);

    /// @brief Report a trap with source location @p loc within function @p fn and
    /// block @p block.
    /// @details The common path does not return, but tests and embedders may
    ///          override `vm_trap()` with an observer that records the trap and
    ///          continues execution.
    /// @param kind Runtime trap classification.
    /// @param msg Human-readable diagnostic.
    /// @param loc Source location associated with the trap.
    /// @param fn Active function name.
    /// @param block Active block label.
    /// @param code Runtime-specific numeric error code.
    static void trap(TrapKind kind,
                     const std::string &msg,
                     const il::support::SourceLoc &loc,
                     const std::string &fn,
                     const std::string &block,
                     int32_t code = 0);

    /// @brief Dispatch a VM/runtime trap through the installed thread-local interceptor.
    /// @details No-ops when no interceptor is active; otherwise throws RuntimeTrapSignal.
    /// @param kind Runtime trap classification.
    /// @param code Runtime-specific numeric error code.
    /// @param msg Human-readable diagnostic.
    /// @param loc Source location associated with the trap.
    /// @param fn Active function name.
    /// @param block Active block label.
    static void interceptTrap(TrapKind kind,
                              int32_t code,
                              const std::string &msg,
                              const il::support::SourceLoc &loc,
                              const std::string &fn,
                              const std::string &block);

    /// @brief Access the runtime call context active on the current thread.
    /// @return Pointer to the call context when a runtime call is executing; nullptr otherwise.
    static const RuntimeCallContext *activeContext();

    /// @brief Indicate whether a VM instance is actively executing on this thread.
    /// @return @c true when a VM is active on the current thread.
    static bool hasActiveVm();

    /// @brief Retrieve the per-VM extern registry for the active VM, if any.
    /// @return Pointer to the active VM's registry, or nullptr if no VM is active
    ///         or the active VM has no per-VM registry assigned.
    static ExternRegistry *activeVmRegistry();

    //=========================================================================
    // Extern Registry (Process-Global)
    //=========================================================================
    // These methods operate on the process-global extern registry shared by
    // all VM instances. For explicit registry control, use the free functions
    // registerExternIn(), unregisterExternIn(), findExternIn() instead.

    /// @brief Register an external function in the process-global registry.
    /// @param ext Descriptor of the external function to register.
    /// @note Equivalent to `registerExternIn(processGlobalExternRegistry(), ext)`.
    static void registerExtern(const ExternDesc &ext);

    /// @brief Unregister an external function from the process-global registry.
    /// @param name Name of the function to unregister (case-insensitive).
    /// @return @c true if a function was unregistered; @c false if not found.
    /// @note Equivalent to `unregisterExternIn(processGlobalExternRegistry(), name)`.
    static bool unregisterExtern(std::string_view name);

    /// @brief Find an external function in the process-global registry.
    /// @param name Name of the function to find (case-insensitive).
    /// @return Pointer to the descriptor if found; nullptr otherwise.
    /// @note Equivalent to `findExternIn(processGlobalExternRegistry(), name)`.
    static const ExternDesc *findExtern(std::string_view name);
};

} // namespace il::vm
