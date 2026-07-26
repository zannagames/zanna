//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/ThreadsRuntime.cpp
// Purpose: VM-side implementations for Zanna.Threads runtime helpers that need
//          VM-aware behavior (notably Thread.Start entry pointers).
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief VM-aware runtime helpers for Zanna.Threads.
/// @details Implements the Thread.Start bridge so Zanna threads can invoke
///          IL entry functions directly when running inside the VM.  Async,
///          pool, and parallel callback APIs either create child VMs or provide
///          deterministic sequential equivalents where native callback pointers
///          cannot safely represent interpreted functions.

#include "vm/RuntimeBridge.hpp"

#include "il/core/Module.hpp"
#include "il/runtime/RuntimeNames.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "rt.hpp"
#include "rt_async.h"
#include "rt_future.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_seq.h"
#include "rt_threadpool.h"
#include "rt_threads.h"
#include "vm/OpHandlerAccess.hpp"
#include "vm/VM.hpp"
#include "vm/VMContext.hpp"

#include "support/small_vector.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <string>

namespace il::vm {
namespace {

using il::runtime::signatures::make_signature;
using il::runtime::signatures::SigParam;

/// @brief Payload passed to the thread entry trampoline.
/// @details Captures the module, program state, entry function, and user arg
///          so a new VM can be created and invoked on the target function.
struct VmThreadStartPayload {
    const il::core::Module *module = nullptr; ///< Module owning @ref entry.
    std::shared_ptr<VM::ProgramState> program; ///< Shared mutable VM state.
    ExternRegistry *externRegistry = nullptr; ///< Retained external registry.
    const il::core::Function *entry = nullptr; ///< Resolved worker function.
    void *arg = nullptr; ///< Optional object argument.
    bool ownsArg = false; ///< Whether payload retains @ref arg.
};

/// @brief Payload passed to VM-backed Async.Run worker threads.
/// @details Extends the basic thread payload with a promise reference owned by
///          the worker thread until it resolves the asynchronous result.
struct VmAsyncRunPayload {
    const il::core::Module *module = nullptr; ///< Module owning @ref entry.
    std::shared_ptr<VM::ProgramState> program; ///< Shared mutable VM state.
    ExternRegistry *externRegistry = nullptr; ///< Retained external registry.
    const il::core::Function *entry = nullptr; ///< Resolved async worker.
    void *arg = nullptr; ///< Worker environment object.
    bool ownsArg = false; ///< Whether payload retains @ref arg.
    void *promise = nullptr; ///< Promise resolved by the worker.
};

/// @brief Release all resources owned by a VM thread-start payload.
/// @param payload Payload to release; @c nullptr is ignored.
static void releaseThreadStartPayload(VmThreadStartPayload *payload) {
    if (!payload)
        return;
    if (payload->ownsArg && payload->arg) {
        if (rt_obj_release_check0(payload->arg))
            rt_obj_free(payload->arg);
        payload->arg = nullptr;
        payload->ownsArg = false;
    }
    releaseExternRegistry(payload->externRegistry);
    payload->externRegistry = nullptr;
    delete payload;
}

/// @brief No-op interceptor used to convert child-VM traps into catchable signals.
/// @details Trap escalation is supplied by @ref ScopedRuntimeTrapInterceptor;
///          the observer itself intentionally records no additional state.
static void vmThreadTrapPassthrough(const RuntimeTrapSignal &, void *) {}

/// @brief Release an async payload's retained worker argument.
/// @param payload Payload whose argument ownership is cleared.
static void releaseAsyncRunArg(VmAsyncRunPayload *payload) {
    if (!payload || !payload->ownsArg || !payload->arg)
        return;
    if (rt_obj_release_check0(payload->arg))
        rt_obj_free(payload->arg);
    payload->arg = nullptr;
    payload->ownsArg = false;
}

/// @brief Execute and release a thread payload while capturing failure text.
/// @param payload Owned payload consumed on every return path.
/// @param errorBuf Optional destination for a null-terminated failure message.
/// @param errorBufSize Capacity of @p errorBuf in bytes.
/// @return @c true when the entry completed; @c false for invalid payload,
///         runtime trap, or exception.
static bool runVmThreadPayload(VmThreadStartPayload *payload, char *errorBuf, size_t errorBufSize) {
    if (errorBuf && errorBufSize > 0)
        errorBuf[0] = '\0';
    if (!payload || !payload->module || !payload->entry) {
        if (errorBuf && errorBufSize > 0)
            std::snprintf(errorBuf, errorBufSize, "%s", "Thread.StartSafe: invalid entry");
        releaseThreadStartPayload(payload);
        return false;
    }

    try {
        VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);
        ScopedRuntimeTrapInterceptor trapInterceptor(&vmThreadTrapPassthrough, nullptr);

        il::support::SmallVector<Slot, 2> args;
        if (payload->entry->params.size() == 1) {
            Slot s{};
            s.ptr = payload->arg;
            args.push_back(s);
        }

        detail::VMAccess::callFunction(vm, *payload->entry, args);
    } catch (const RuntimeTrapSignal &signal) {
        if (errorBuf && errorBufSize > 0) {
            std::snprintf(errorBuf,
                          errorBufSize,
                          "%s",
                          signal.message.empty() ? "Thread.StartSafe: trapped VM worker"
                                                 : signal.message.c_str());
        }
        releaseThreadStartPayload(payload);
        return false;
    } catch (...) {
        if (errorBuf && errorBufSize > 0)
            std::snprintf(errorBuf, errorBufSize, "%s", "Thread.StartSafe: unhandled exception");
        releaseThreadStartPayload(payload);
        return false;
    }

    releaseThreadStartPayload(payload);
    return true;
}

/// @brief Thread entry trampoline for VM-backed Thread.Start.
/// @details Validates the payload, creates a new VM bound to the same program
///          state, and invokes the entry function. Any unexpected exception
///          aborts the runtime to avoid silent thread failures.
/// @param raw Opaque pointer to a @ref VmThreadStartPayload.
extern "C" void vm_thread_entry_trampoline(void *raw) {
    VmThreadStartPayload *payload = static_cast<VmThreadStartPayload *>(raw);
    if (!payload || !payload->module || !payload->entry) {
        releaseThreadStartPayload(payload);
        rt_abort("Thread.Start: invalid entry");
    }

    try {
        VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);

        il::support::SmallVector<Slot, 2> args;
        if (payload->entry->params.size() == 1) {
            Slot s{};
            s.ptr = payload->arg;
            args.push_back(s);
        }

        detail::VMAccess::callFunction(vm, *payload->entry, args);
    } catch (const RuntimeTrapSignal &signal) {
        releaseThreadStartPayload(payload);
        const std::string message =
            signal.message.empty() ? "Thread.Start: trapped VM worker" : signal.message;
        rt_abort(message.c_str());
    } catch (const std::exception &ex) {
        releaseThreadStartPayload(payload);
        const std::string message = std::string("Thread.Start: unhandled exception: ") + ex.what();
        rt_abort(message.c_str());
    } catch (...) {
        releaseThreadStartPayload(payload);
        rt_abort("Thread.Start: unhandled non-standard exception");
    }

    releaseThreadStartPayload(payload);
}

/// @brief Resolve a function pointer into a module function.
/// @details The runtime passes a raw function pointer; this helper verifies it
///          matches one of the module's function objects before use.
/// @param module Module containing candidate functions.
/// @param entry Raw pointer supplied by the runtime.
/// @return Pointer to the matching function, or nullptr if invalid.
static const il::core::Function *resolveEntryFunction(const il::core::Module &module, void *entry) {
    if (!entry)
        return nullptr;
    const auto *candidate = static_cast<const il::core::Function *>(entry);
    for (const auto &fn : module.functions) {
        if (&fn == candidate)
            return &fn;
    }
    return nullptr;
}

/// @brief Validate the signature of a thread entry function.
/// @details Thread entry functions must return void and accept either zero
///          parameters or a single pointer parameter. Violations trap with a
///          diagnostic message.
/// @param fn Function to validate.
static void validateEntrySignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Void)
        rt_trap("Thread.Start: invalid entry signature");
    if (fn.params.empty())
        return;
    if (fn.params.size() == 1 && fn.params[0].type.kind == Kind::Ptr)
        return;
    rt_trap("Thread.Start: invalid entry signature");
}

/// @brief Validate the signature of an Async.Run worker entry function.
/// @details Async worker trampolines lowered from Zia async functions must
///          return an object pointer and accept exactly one environment pointer.
/// @param fn Resolved worker function to validate.
static void validateAsyncEntrySignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Ptr)
        rt_trap("Async.Run: invalid entry signature");
    if (fn.params.size() == 1 && fn.params[0].type.kind == Kind::Ptr)
        return;
    rt_trap("Async.Run: invalid entry signature");
}

/// @brief Runtime bridge handler for Zanna.Threads.Thread.Start.
/// @details When running inside a VM, this handler validates the entry function
///          pointer, constructs a thread payload, and spawns a native thread
///          that executes the IL entry via the trampoline. Outside the VM it
///          forwards directly to @ref rt_thread_start.
/// @param args Argument array provided by the runtime bridge.
/// @param result Optional out-parameter to receive the thread handle.
static void threads_thread_start_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry)
        rt_trap("Thread.Start: null entry");

    VM *parentVm = activeVMInstance();
    if (!parentVm) {
        void *thread = rt_thread_start(entry, arg);
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    std::shared_ptr<VM::ProgramState> program = parentVm->programState();
    if (!program)
        rt_trap("Thread.Start: invalid runtime state");

    const il::core::Module &module = parentVm->module();
    const il::core::Function *entryFn = resolveEntryFunction(module, entry);
    if (!entryFn)
        rt_trap("Thread.Start: invalid entry");
    validateEntrySignature(*entryFn);

    auto *payload = new (std::nothrow) VmThreadStartPayload{
        &module, std::move(program), parentVm->externRegistry(), entryFn, arg, false};
    if (!payload)
        rt_trap("Thread.Start: payload allocation failed");
    retainExternRegistry(payload->externRegistry);
    void *thread = rt_thread_start(reinterpret_cast<void *>(&vm_thread_entry_trampoline), payload);
    if (!thread) {
        releaseThreadStartPayload(payload);
        rt_trap("Thread.Start: failed to create thread");
    }
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Runtime bridge handler for Zanna.Threads.Thread.StartOwned.
/// @details VM variant retains the managed argument until the IL entry returns.
/// @param args Runtime argument-storage array containing entry and argument.
/// @param result Optional storage that receives the native thread handle.
static void threads_thread_start_owned_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry)
        rt_trap("Thread.StartOwned: null entry");

    VM *parentVm = activeVMInstance();
    if (!parentVm) {
        void *thread = rt_thread_start_owned(entry, arg);
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    std::shared_ptr<VM::ProgramState> program = parentVm->programState();
    if (!program)
        rt_trap("Thread.StartOwned: invalid runtime state");

    const il::core::Module &module = parentVm->module();
    const il::core::Function *entryFn = resolveEntryFunction(module, entry);
    if (!entryFn)
        rt_trap("Thread.StartOwned: invalid entry");
    validateEntrySignature(*entryFn);

    auto *payload = new (std::nothrow) VmThreadStartPayload{
        &module, std::move(program), parentVm->externRegistry(), entryFn, arg, arg != nullptr};
    if (!payload)
        rt_trap("Thread.StartOwned: payload allocation failed");
    if (payload->ownsArg)
        rt_obj_retain_maybe(arg);
    retainExternRegistry(payload->externRegistry);
    void *thread = rt_thread_start(reinterpret_cast<void *>(&vm_thread_entry_trampoline), payload);
    if (!thread) {
        releaseThreadStartPayload(payload);
        rt_trap("Thread.StartOwned: failed to create thread");
    }
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Safe thread entry trampoline that captures trap errors.
/// @details Like vm_thread_entry_trampoline but wraps execution in a
///          setjmp/longjmp recovery point so traps are captured instead of
///          terminating the process.
/// @param raw Opaque pointer to a @ref VmThreadStartPayload.
extern "C" void vm_thread_safe_entry_trampoline(void *raw) {
    char error[512];
    if (!runVmThreadPayload(static_cast<VmThreadStartPayload *>(raw), error, sizeof(error)))
        rt_trap(error[0] ? error : "Thread.StartSafe: trapped VM worker");
}

/// @brief Runtime bridge handler for Zanna.Threads.Thread.StartSafe.
/// @details Like threads_thread_start_handler but uses the safe entry trampoline
///          that wraps execution in trap recovery via setjmp/longjmp.
/// @param args Runtime argument-storage array containing entry and argument.
/// @param result Optional storage that receives the safe thread handle.
static void threads_thread_start_safe_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry)
        rt_trap("Thread.StartSafe: null entry");

    VM *parentVm = activeVMInstance();
    if (!parentVm) {
        void *thread = rt_thread_start_safe(entry, arg);
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    std::shared_ptr<VM::ProgramState> program = parentVm->programState();
    if (!program)
        rt_trap("Thread.StartSafe: invalid runtime state");

    const il::core::Module &module = parentVm->module();
    const il::core::Function *entryFn = resolveEntryFunction(module, entry);
    if (!entryFn)
        rt_trap("Thread.StartSafe: invalid entry");
    validateEntrySignature(*entryFn);

    auto *payload = new (std::nothrow) VmThreadStartPayload{
        &module, std::move(program), parentVm->externRegistry(), entryFn, arg, false};
    if (!payload)
        rt_trap("Thread.StartSafe: payload allocation failed");
    retainExternRegistry(payload->externRegistry);
    void *thread =
        rt_thread_start_safe(reinterpret_cast<void *>(&vm_thread_safe_entry_trampoline), payload);
    if (!thread) {
        releaseThreadStartPayload(payload);
        rt_trap("Thread.StartSafe: failed to create thread");
    }
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Runtime bridge handler for Zanna.Threads.Thread.StartSafeOwned.
/// @details VM variant combines safe trap capture with retaining the managed argument.
/// @param args Runtime argument-storage array containing entry and argument.
/// @param result Optional storage that receives the safe thread handle.
static void threads_thread_start_safe_owned_handler(void **args, void *result) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry)
        rt_trap("Thread.StartSafeOwned: null entry");

    VM *parentVm = activeVMInstance();
    if (!parentVm) {
        void *thread = rt_thread_start_safe_owned(entry, arg);
        if (result)
            *reinterpret_cast<void **>(result) = thread;
        return;
    }

    std::shared_ptr<VM::ProgramState> program = parentVm->programState();
    if (!program)
        rt_trap("Thread.StartSafeOwned: invalid runtime state");

    const il::core::Module &module = parentVm->module();
    const il::core::Function *entryFn = resolveEntryFunction(module, entry);
    if (!entryFn)
        rt_trap("Thread.StartSafeOwned: invalid entry");
    validateEntrySignature(*entryFn);

    auto *payload = new (std::nothrow) VmThreadStartPayload{
        &module, std::move(program), parentVm->externRegistry(), entryFn, arg, arg != nullptr};
    if (!payload)
        rt_trap("Thread.StartSafeOwned: payload allocation failed");
    if (payload->ownsArg)
        rt_obj_retain_maybe(arg);
    retainExternRegistry(payload->externRegistry);
    void *thread =
        rt_thread_start_safe(reinterpret_cast<void *>(&vm_thread_safe_entry_trampoline), payload);
    if (!thread) {
        releaseThreadStartPayload(payload);
        rt_trap("Thread.StartSafeOwned: failed to create thread");
    }
    if (result)
        *reinterpret_cast<void **>(result) = thread;
}

/// @brief Async worker trampoline for VM-backed Async.Run.
/// @details Executes the IL worker function in a child VM, then resolves the
///          promise with the returned object pointer.
/// @param raw Owned opaque pointer to a @ref VmAsyncRunPayload.
extern "C" void vm_async_run_entry_trampoline(void *raw) {
    VmAsyncRunPayload *payload = static_cast<VmAsyncRunPayload *>(raw);
    if (!payload || !payload->module || !payload->entry || !payload->promise) {
        if (payload) {
            releaseAsyncRunArg(payload);
            releaseExternRegistry(payload->externRegistry);
        }
        delete payload;
        rt_abort("Async.Run: invalid entry");
    }

    try {
        VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);

        il::support::SmallVector<Slot, 2> args;
        Slot argSlot{};
        argSlot.ptr = payload->arg;
        args.push_back(argSlot);

        Slot result = detail::VMAccess::callFunction(vm, *payload->entry, args);
        if (payload->ownsArg && result.ptr == payload->arg) {
            payload->arg = nullptr;
            payload->ownsArg = false;
            rt_promise_set_transferred(payload->promise, result.ptr);
        } else {
            releaseAsyncRunArg(payload);
            // Worker VMs unwind immediately after resolving; the Future must retain the result.
            rt_promise_set_owned(payload->promise, result.ptr);
        }
    } catch (const RuntimeTrapSignal &signal) {
        const char *message =
            signal.message.empty() ? "Async.Run: trapped VM worker" : signal.message.c_str();
        releaseAsyncRunArg(payload);
        rt_promise_set_error(payload->promise, rt_const_cstr(message));
    } catch (const std::exception &ex) {
        const std::string message = std::string("Async.Run: unhandled exception: ") + ex.what();
        releaseAsyncRunArg(payload);
        rt_promise_set_error(payload->promise, rt_const_cstr(message.c_str()));
    } catch (...) {
        releaseAsyncRunArg(payload);
        rt_promise_set_error(payload->promise,
                             rt_const_cstr("Async.Run: unhandled non-standard exception"));
    }

    if (rt_obj_release_check0(payload->promise))
        rt_obj_free(payload->promise);
    releaseExternRegistry(payload->externRegistry);
    delete payload;
}

/// @brief Runtime bridge handler for Zanna.Threads.Async.Run.
/// @details Uses the native runtime helper outside the VM, but when an IL
///          function pointer is supplied from VM execution it spawns a child VM
///          and resolves a Future with that worker's returned object.
/// @param args Runtime argument-storage array containing entry and environment.
/// @param result Optional storage that receives the future.
/// @param owned Whether the payload retains the environment until completion.
static void threads_async_run_impl(void **args, void *result, bool owned) {
    void *entry = nullptr;
    void *arg = nullptr;
    if (args && args[0])
        entry = *reinterpret_cast<void **>(args[0]);
    if (args && args[1])
        arg = *reinterpret_cast<void **>(args[1]);

    if (!entry)
        rt_trap("Async.Run: null entry");

    VM *parentVm = activeVMInstance();
    if (!parentVm) {
        void *future = owned ? rt_async_run_owned(entry, arg) : rt_async_run(entry, arg);
        if (result)
            *reinterpret_cast<void **>(result) = future;
        return;
    }

    std::shared_ptr<VM::ProgramState> program = parentVm->programState();
    if (!program)
        rt_trap("Async.Run: invalid runtime state");

    const il::core::Module &module = parentVm->module();
    const il::core::Function *entryFn = resolveEntryFunction(module, entry);
    if (!entryFn)
        rt_trap("Async.Run: invalid entry");
    validateAsyncEntrySignature(*entryFn);

    void *promise = rt_promise_new();
    void *future = rt_promise_get_future(promise);

    // Ownership follows the variant (VDOC-127): Run borrows the arg exactly
    // like the native path (the caller keeps it alive), RunOwned retains it
    // until the worker completes.
    auto *payload = new (std::nothrow) VmAsyncRunPayload{&module,
                                                         std::move(program),
                                                         parentVm->externRegistry(),
                                                         entryFn,
                                                         arg,
                                                         owned && arg != nullptr,
                                                         promise};
    if (!payload) {
        rt_promise_set_error(promise, rt_const_cstr("Async.Run: payload allocation failed"));
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        if (result)
            *reinterpret_cast<void **>(result) = future;
        return;
    }
    if (payload->ownsArg)
        rt_obj_retain_maybe(arg);
    retainExternRegistry(payload->externRegistry);
    void *thread =
        rt_thread_start(reinterpret_cast<void *>(&vm_async_run_entry_trampoline), payload);
    if (!thread) {
        releaseAsyncRunArg(payload);
        releaseExternRegistry(payload->externRegistry);
        delete payload;
        rt_promise_set_error(promise, rt_const_cstr("Async.Run: failed to create thread"));
        if (rt_obj_release_check0(promise))
            rt_obj_free(promise);
        if (result)
            *reinterpret_cast<void **>(result) = future;
        return;
    }

    if (rt_obj_release_check0(thread))
        rt_obj_free(thread);
    if (result)
        *reinterpret_cast<void **>(result) = future;
}

/// @brief Runtime bridge handler for Zanna.Threads.Async.Run (borrowed arg).
/// @param args Runtime argument-storage array containing entry and environment.
/// @param result Optional storage that receives the future.
static void threads_async_run_handler(void **args, void *result) {
    threads_async_run_impl(args, result, /*owned=*/false);
}

/// @brief Runtime bridge handler for Zanna.Threads.Async.RunOwned (owned arg).
/// @param args Runtime argument-storage array containing entry and environment.
/// @param result Optional storage that receives the future.
static void threads_async_run_owned_handler(void **args, void *result) {
    threads_async_run_impl(args, result, /*owned=*/true);
}

/// @brief Trap for Async callback variants without a managed VM bridge
///        (VDOC-127): the native implementations would cast the opaque VM
///        function value to a native pointer — undefined behavior.
/// @param api Public API name included in the diagnostic.
/// @return @c true after reporting unsupported interpreted execution; @c false
///         when native callback forwarding is safe.
static bool vmAsyncCallbackUnsupported(const char *api) {
    if (activeVMInstance()) {
        rt_trap((std::string(api) +
                 ": callback execution is not supported on the interpreted backends; "
                 "compile to native to run it")
                    .c_str());
        return true;
    }
    return false;
}

/// @brief Forward borrowed-argument cancellable async execution on native backends.
/// @param args Runtime argument-storage array containing entry, argument, and
///        cancellation token.
/// @param result Optional storage that receives the future.
static void threads_async_run_cancellable_handler(void **args, void *result) {
    if (vmAsyncCallbackUnsupported("Async.RunCancellable"))
        return;
    void *entry = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *arg = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *token = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *future = rt_async_run_cancellable(entry, arg, token);
    if (result)
        *reinterpret_cast<void **>(result) = future;
}

/// @brief Forward owned-argument cancellable async execution on native backends.
/// @param args Runtime argument-storage array containing entry, argument, and
///        cancellation token.
/// @param result Optional storage that receives the future.
static void threads_async_run_cancellable_owned_handler(void **args, void *result) {
    if (vmAsyncCallbackUnsupported("Async.RunCancellableOwned"))
        return;
    void *entry = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *arg = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *token = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *future = rt_async_run_cancellable_owned(entry, arg, token);
    if (result)
        *reinterpret_cast<void **>(result) = future;
}

/// @brief Forward borrowed-argument future mapping on native backends.
/// @param args Runtime argument-storage array containing future, callback, and
///        callback argument.
/// @param result Optional storage that receives the mapped future.
static void threads_async_map_handler(void **args, void *result) {
    if (vmAsyncCallbackUnsupported("Async.Map"))
        return;
    void *future = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *fn = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *mapped = rt_async_map(future, fn, arg);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Forward owned-argument future mapping on native backends.
/// @param args Runtime argument-storage array containing future, callback, and
///        callback argument.
/// @param result Optional storage that receives the mapped future.
static void threads_async_map_owned_handler(void **args, void *result) {
    if (vmAsyncCallbackUnsupported("Async.MapOwned"))
        return;
    void *future = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *fn = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *mapped = rt_async_map_owned(future, fn, arg);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Run an index-range callback sequentially on the active VM.
/// @details Parallel.For iterations are independent, so sequential execution on the VM yields the
///          same result as the native parallel path — the VM trades the parallelism for a
///          single-threaded, debuggable run rather than trapping. The callback must be
///          `(Integer) -> Unit` (one i64 parameter, void return).
/// @param vm Active VM used for callback execution.
/// @param start Inclusive first index.
/// @param end Exclusive final index.
/// @param func Raw IL callback address.
/// @param api Public API name used in trap diagnostics.
static void runVmParallelForRange(VM &vm, int64_t start, int64_t end, void *func, const char *api) {
    const il::core::Function *fn = resolveEntryFunction(vm.module(), func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Void || fn->params.size() != 1 ||
        fn->params[0].type.kind != Kind::I64) {
        rt_trap((std::string(api) + ": callback must be (Integer) -> Unit").c_str());
        return;
    }
    for (int64_t i = start; i < end; ++i) {
        il::support::SmallVector<Slot, 1> callArgs;
        Slot s{};
        s.i64 = i;
        callArgs.push_back(s);
        detail::VMAccess::callFunction(vm, *fn, callArgs);
    }
}

/// @brief Run a pool task callback synchronously on the active VM.
/// @details The VM has no worker pool, so the task runs immediately on the calling thread instead
///          of trapping. The callback must be `(Ptr) -> Unit` or take no parameters.
/// @param vm Active VM used for callback execution.
/// @param callback Raw IL callback address.
/// @param arg Optional object argument.
/// @param api Public API name used in trap diagnostics.
static void runVmPoolTask(VM &vm, void *callback, void *arg, const char *api) {
    const il::core::Function *fn = resolveEntryFunction(vm.module(), callback);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Void || fn->params.size() > 1 ||
        (fn->params.size() == 1 && fn->params[0].type.kind != Kind::Ptr)) {
        rt_trap((std::string(api) + ": callback must be (Ptr) -> Unit").c_str());
        return;
    }
    il::support::SmallVector<Slot, 1> taskArgs;
    if (fn->params.size() == 1) {
        Slot s{};
        s.ptr = arg;
        taskArgs.push_back(s);
    }
    detail::VMAccess::callFunction(vm, *fn, taskArgs);
}

/// @brief Invoke a sequence of zero-argument callbacks sequentially on the active VM.
/// @details Parallel.Invoke runs a list of independent `() -> Unit` actions; the native path fans
///          them across the pool, but on the VM they run in order on the calling thread rather
///          than trapping. Each sequence element is a VM function value; a non-resolvable entry or
///          one with the wrong arity/return type traps with an explicit message.
/// @param vm Active VM used for callback execution.
/// @param funcs Runtime sequence containing raw IL function addresses.
/// @param api Public API name used in trap diagnostics.
static void runVmInvokeFunctions(VM &vm, void *funcs, const char *api) {
    if (!funcs)
        return;
    const int64_t count = rt_seq_len(funcs);
    if (count < 0) {
        rt_trap((std::string(api) + ": negative sequence length").c_str());
        return;
    }
    using Kind = il::core::Type::Kind;
    for (int64_t i = 0; i < count; ++i) {
        const il::core::Function *fn = resolveEntryFunction(vm.module(), rt_seq_get(funcs, i));
        if (!fn) {
            rt_trap((std::string(api) + ": invalid callback function").c_str());
            return;
        }
        if (fn->retType.kind != Kind::Void || !fn->params.empty()) {
            rt_trap((std::string(api) + ": callbacks must be () -> Unit").c_str());
            return;
        }
        il::support::SmallVector<Slot, 1> noArgs;
        detail::VMAccess::callFunction(vm, *fn, noArgs);
    }
}

/// @brief Run a `(Ptr) -> Unit` callback over each Seq element on the VM.
/// @details Sequential equivalent of the native Parallel.ForEach (VDOC-126):
///          same visible effects, single-threaded ordering.
/// @param vm Active VM used for callback execution.
/// @param seq Runtime sequence to traverse; null is treated as empty.
/// @param func Raw IL callback address.
/// @param api Public API name used in trap diagnostics.
static void runVmSeqForEach(VM &vm, void *seq, void *func, const char *api) {
    if (!seq)
        return;
    const il::core::Function *fn = resolveEntryFunction(vm.module(), func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Void || fn->params.size() != 1 ||
        fn->params[0].type.kind != Kind::Ptr) {
        rt_trap((std::string(api) + ": callback must be (Object) -> Unit").c_str());
        return;
    }
    const int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        il::support::SmallVector<Slot, 1> callArgs;
        Slot s{};
        s.ptr = rt_seq_get(seq, i);
        callArgs.push_back(s);
        detail::VMAccess::callFunction(vm, *fn, callArgs);
    }
}

/// @brief Map a Seq through a `(Ptr) -> Ptr` callback on the VM.
/// @details Sequential equivalent of the native Parallel.Map (VDOC-126);
///          returns an owning Seq of the mapped values in order.
/// @param vm Active VM used for callback execution.
/// @param seq Runtime sequence to map; null is treated as empty.
/// @param func Raw IL mapping callback address.
/// @param api Public API name used in trap diagnostics.
/// @return Owning runtime sequence of mapped objects.
static void *runVmSeqMap(VM &vm, void *seq, void *func, const char *api) {
    void *out = rt_seq_new();
    rt_seq_set_owns_elements(out, 1);
    if (!seq)
        return out;
    const il::core::Function *fn = resolveEntryFunction(vm.module(), func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return out;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Ptr || fn->params.size() != 1 ||
        fn->params[0].type.kind != Kind::Ptr) {
        rt_trap((std::string(api) + ": callback must be (Object) -> Object").c_str());
        return out;
    }
    const int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        il::support::SmallVector<Slot, 1> callArgs;
        Slot s{};
        s.ptr = rt_seq_get(seq, i);
        callArgs.push_back(s);
        Slot r = detail::VMAccess::callFunction(vm, *fn, callArgs);
        rt_seq_push(out, r.ptr);
    }
    return out;
}

/// @brief Fold a Seq through a `(Ptr, Ptr) -> Ptr` callback on the VM.
/// @details Sequential left fold matching the native Parallel.Reduce result
///          for associative reducers (VDOC-126).
/// @param vm Active VM used for callback execution.
/// @param seq Runtime sequence to reduce; null returns @p identity.
/// @param func Raw IL reducer callback address.
/// @param identity Initial accumulator.
/// @param api Public API name used in trap diagnostics.
/// @return Final accumulator returned by the sequential fold.
static void *runVmSeqReduce(VM &vm, void *seq, void *func, void *identity, const char *api) {
    if (!seq)
        return identity;
    const il::core::Function *fn = resolveEntryFunction(vm.module(), func);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return identity;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Ptr || fn->params.size() != 2 ||
        fn->params[0].type.kind != Kind::Ptr || fn->params[1].type.kind != Kind::Ptr) {
        rt_trap((std::string(api) + ": callback must be (Object, Object) -> Object").c_str());
        return identity;
    }
    void *acc = identity;
    const int64_t count = rt_seq_len(seq);
    for (int64_t i = 0; i < count; ++i) {
        il::support::SmallVector<Slot, 2> callArgs;
        Slot a{};
        a.ptr = acc;
        callArgs.push_back(a);
        Slot b{};
        b.ptr = rt_seq_get(seq, i);
        callArgs.push_back(b);
        Slot r = detail::VMAccess::callFunction(vm, *fn, callArgs);
        acc = r.ptr;
    }
    return acc;
}

/// @brief Submit a borrowed-argument pool task or run it synchronously on the VM.
/// @param args Runtime argument-storage array containing pool, callback, and argument.
/// @param result Optional storage that receives the submission boolean.
static void threads_pool_submit_handler(void **args, void *result) {
    void *pool = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;

    if (VM *vm = activeVMInstance()) {
        // No worker pool on the VM: run the task synchronously, preserving
        // "submit, then it runs" semantics for dev/debug instead of trapping.
        // Pool state is still honored (VDOC-125): a shut-down or invalid
        // pool rejects the submission exactly like the native backend.
        if (!pool || rt_threadpool_get_is_shutdown(pool)) {
            if (result)
                *reinterpret_cast<int8_t *>(result) = 0;
            return;
        }
        runVmPoolTask(*vm, callback, arg, "Pool.Submit");
        if (result)
            *reinterpret_cast<int8_t *>(result) = 1;
        return;
    }

    int8_t submitted = rt_threadpool_submit(pool, callback, arg);
    if (result)
        *reinterpret_cast<int8_t *>(result) = submitted;
}

/// @brief SubmitOwned bridge: on the VM the task runs synchronously, so the
///        pool-side retain/release nets out within the call (VDOC-128).
/// @param args Runtime argument-storage array containing pool, callback, and argument.
/// @param result Optional storage that receives the submission boolean.
static void threads_pool_submit_owned_handler(void **args, void *result) {
    void *pool = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *arg = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;

    if (VM *vm = activeVMInstance()) {
        if (!pool || rt_threadpool_get_is_shutdown(pool)) {
            if (result)
                *reinterpret_cast<int8_t *>(result) = 0;
            return;
        }
        runVmPoolTask(*vm, callback, arg, "Pool.SubmitOwned");
        if (result)
            *reinterpret_cast<int8_t *>(result) = 1;
        return;
    }

    int8_t submitted = rt_threadpool_submit_owned(pool, callback, arg);
    if (result)
        *reinterpret_cast<int8_t *>(result) = submitted;
}

/// @brief Execute Parallel.ForEach natively or sequentially on the active VM.
/// @param args Runtime argument-storage array containing sequence and callback.
/// @param result Unused result-storage pointer.
static void threads_parallel_foreach_handler(void **args, void *result) {
    (void)result;
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        runVmSeqForEach(*vm, seq, func, "Parallel.ForEach");
        return;
    }
    rt_parallel_foreach(seq, func);
}

/// @brief Execute pool-qualified Parallel.ForEach with a sequential VM fallback.
/// @param args Runtime argument-storage array containing sequence, callback, and pool.
/// @param result Unused result-storage pointer.
static void threads_parallel_foreach_pool_handler(void **args, void *result) {
    (void)result;
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *pool = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        // The pool is a native concurrency hint; on the VM the walk is sequential.
        (void)pool;
        runVmSeqForEach(*vm, seq, func, "Parallel.ForEachPool");
        return;
    }
    rt_parallel_foreach_pool(seq, func, pool);
}

/// @brief Execute Parallel.Map natively or sequentially on the active VM.
/// @param args Runtime argument-storage array containing sequence and callback.
/// @param result Optional storage that receives the mapped sequence.
static void threads_parallel_map_handler(void **args, void *result) {
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        void *vmMapped = runVmSeqMap(*vm, seq, func, "Parallel.Map");
        if (result)
            *reinterpret_cast<void **>(result) = vmMapped;
        return;
    }
    void *mapped = rt_parallel_map(seq, func);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Execute pool-qualified Parallel.Map with a sequential VM fallback.
/// @param args Runtime argument-storage array containing sequence, callback, and pool.
/// @param result Optional storage that receives the mapped sequence.
static void threads_parallel_map_pool_handler(void **args, void *result) {
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *pool = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        (void)pool;
        void *vmMapped = runVmSeqMap(*vm, seq, func, "Parallel.MapPool");
        if (result)
            *reinterpret_cast<void **>(result) = vmMapped;
        return;
    }
    void *mapped = rt_parallel_map_pool(seq, func, pool);
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Execute Parallel.Invoke natively or sequentially on the active VM.
/// @param args Runtime argument-storage array containing the callback sequence.
/// @param result Unused result-storage pointer.
static void threads_parallel_invoke_handler(void **args, void *result) {
    (void)result;
    void *funcs = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        runVmInvokeFunctions(*vm, funcs, "Parallel.Invoke");
        return;
    }
    rt_parallel_invoke(funcs);
}

/// @brief Execute pool-qualified Parallel.Invoke with a sequential VM fallback.
/// @param args Runtime argument-storage array containing callbacks and pool.
/// @param result Unused result-storage pointer.
static void threads_parallel_invoke_pool_handler(void **args, void *result) {
    (void)result;
    void *funcs = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *pool = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        // The pool is a native concurrency hint; on the VM the actions run sequentially.
        (void)pool;
        runVmInvokeFunctions(*vm, funcs, "Parallel.InvokePool");
        return;
    }
    rt_parallel_invoke_pool(funcs, pool);
}

/// @brief Execute Parallel.For natively or sequentially on the active VM.
/// @param args Runtime argument-storage array containing range and callback.
/// @param result Unused result-storage pointer.
static void threads_parallel_for_handler(void **args, void *result) {
    (void)result;
    int64_t start = args && args[0] ? *reinterpret_cast<int64_t *>(args[0]) : 0;
    int64_t end = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
    void *func = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        runVmParallelForRange(*vm, start, end, func, "Parallel.For");
        return;
    }
    rt_parallel_for(start, end, func);
}

/// @brief Execute pool-qualified Parallel.For with a sequential VM fallback.
/// @param args Runtime argument-storage array containing range, callback, and pool.
/// @param result Unused result-storage pointer.
static void threads_parallel_for_pool_handler(void **args, void *result) {
    (void)result;
    int64_t start = args && args[0] ? *reinterpret_cast<int64_t *>(args[0]) : 0;
    int64_t end = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
    void *func = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *pool = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        // The pool is a native concurrency hint; on the VM the range runs sequentially.
        (void)pool;
        runVmParallelForRange(*vm, start, end, func, "Parallel.ForPool");
        return;
    }
    rt_parallel_for_pool(start, end, func, pool);
}

/// @brief Execute Parallel.Reduce natively or sequentially on the active VM.
/// @param args Runtime argument-storage array containing sequence, reducer, and identity.
/// @param result Optional storage that receives the reduced object.
static void threads_parallel_reduce_handler(void **args, void *result) {
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *identity = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        void *vmReduced = runVmSeqReduce(*vm, seq, func, identity, "Parallel.Reduce");
        if (result)
            *reinterpret_cast<void **>(result) = vmReduced;
        return;
    }
    void *reduced = rt_parallel_reduce(seq, func, identity);
    if (result)
        *reinterpret_cast<void **>(result) = reduced;
}

/// @brief Execute pool-qualified Parallel.Reduce with a sequential VM fallback.
/// @param args Runtime argument-storage array containing sequence, reducer,
///        identity, and pool.
/// @param result Optional storage that receives the reduced object.
static void threads_parallel_reduce_pool_handler(void **args, void *result) {
    void *seq = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *func = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *identity = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *pool = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;
    if (VM *vm = activeVMInstance()) {
        (void)pool;
        void *vmReduced = runVmSeqReduce(*vm, seq, func, identity, "Parallel.ReducePool");
        if (result)
            *reinterpret_cast<void **>(result) = vmReduced;
        return;
    }
    void *reduced = rt_parallel_reduce_pool(seq, func, identity, pool);
    if (result)
        *reinterpret_cast<void **>(result) = reduced;
}

} // namespace

/// @brief Register VM-aware thread externals with the runtime bridge.
/// @details Installs the `Zanna.Threads.Thread.Start`, `Thread.StartSafe`, and
///          `Async.Run`
///          handlers so they use the VM trampoline when invoked from managed code.
///          When the BytecodeVM is linked, its unified handlers overwrite these
///          registrations via a static initializer.
void registerThreadsRuntimeExternals() {
    {
        ExternDesc ext;
        ext.name = il::runtime::names::kThreadsThreadStart;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_thread_start_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = il::runtime::names::kThreadsThreadStartOwned;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_thread_start_owned_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = il::runtime::names::kThreadsThreadStartSafe;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_thread_start_safe_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = il::runtime::names::kThreadsThreadStartSafeOwned;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_thread_start_safe_owned_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = il::runtime::names::kThreadsAsyncRun;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_async_run_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Async.RunOwned";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_async_run_owned_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Async.RunCancellable";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_async_run_cancellable_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Async.RunCancellableOwned";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_async_run_cancellable_owned_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Async.Map";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_async_map_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Async.MapOwned";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_async_map_owned_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Pool.Submit";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::I1});
        ext.fn = reinterpret_cast<void *>(&threads_pool_submit_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Pool.SubmitOwned";
        ext.signature =
            make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::I1});
        ext.fn = reinterpret_cast<void *>(&threads_pool_submit_owned_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ForEach";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_foreach_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ForEachPool";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_foreach_pool_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.Map";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_map_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.MapPool";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_map_pool_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.Invoke";
        ext.signature = make_signature(ext.name, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_invoke_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.InvokePool";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_invoke_pool_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.For";
        ext.signature = make_signature(ext.name, {SigParam::I64, SigParam::I64, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_for_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ForPool";
        ext.signature =
            make_signature(ext.name, {SigParam::I64, SigParam::I64, SigParam::Ptr, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_for_pool_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.Reduce";
        ext.signature = make_signature(
            ext.name, {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_reduce_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Threads.Parallel.ReducePool";
        ext.signature = make_signature(ext.name,
                                       {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr, SigParam::Ptr},
                                       {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&threads_parallel_reduce_pool_handler);
        RuntimeBridge::registerExtern(ext);
    }
}

} // namespace il::vm
