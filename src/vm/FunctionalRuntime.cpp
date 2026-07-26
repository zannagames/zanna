//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/FunctionalRuntime.cpp
// Purpose: VM-side bridges for Zanna.Functional runtime helpers that accept
//          managed callback functions (notably Lazy.New deferred suppliers and
//          the Lazy.Map/AndThen combinators).
// Key invariants:
//   - Function values reaching these handlers under the VM are IL function
//     addresses, never C-callable pointers; each handler resolves and executes
//     them through the active VM instead of letting C code call them.
//   - A Lazy created by the VM Lazy.New handler is handle-kind; every accessor
//     handler completes a pending handle before forwarding to the C runtime.
//   - Outside a VM (native execution never routes through these handlers, but
//     other managed contexts may), calls forward to the C implementations.
// Ownership/Lifetime:
//   - Supplier/callback handles reference functions owned by the active VM's
//     module; no retention is required because a pending Lazy cannot outlive
//     the module during a VM run.
// Links: src/runtime/oop/rt_lazy.c, src/vm/ThreadsRuntime.cpp (pattern)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief VM-aware runtime helpers for Zanna.Functional.Lazy.
/// @details Deferred suppliers created by `Lazy.New` are IL functions when the
///          program runs on the VM. The C runtime cannot invoke them, so these
///          handlers intercept the Lazy accessors, execute pending suppliers
///          via the active VM, and publish results with rt_lazy_complete_obj.

#include "vm/RuntimeBridge.hpp"

#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "rt_lazy.h"
#include "rt_option.h"
#include "rt_result.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "vm/OpHandlerAccess.hpp"
#include "vm/VM.hpp"
#include "vm/VMContext.hpp"

#include "support/small_vector.hpp"

#include <cstdint>
#include <string>

namespace il::vm {
namespace {

using il::runtime::signatures::make_signature;
using il::runtime::signatures::SigParam;

/// @brief Resolve a function pointer into a module function.
/// @details The runtime passes a raw function address; this helper verifies it
///          matches one of the module's function objects before use.
/// @param module Module containing candidate functions.
/// @param entry Raw pointer supplied by the runtime.
/// @return Pointer to the matching function, or nullptr if invalid.
static const il::core::Function *resolveCallbackFunction(const il::core::Module &module,
                                                         void *entry) {
    if (!entry)
        return nullptr;
    const auto *candidate = static_cast<const il::core::Function *>(entry);
    for (const auto &fn : module.functions) {
        if (&fn == candidate)
            return &fn;
    }
    return nullptr;
}

/// @brief Execute a zero-argument, object-returning IL supplier on the VM.
/// @param vm Active VM executing the program.
/// @param handle IL function address stored as the Lazy's pending supplier.
/// @param api Public API name used in trap diagnostics.
/// @return Object pointer produced by the supplier.
static void *runVmSupplier(VM &vm, void *handle, const char *api) {
    const il::core::Function *fn = resolveCallbackFunction(vm.module(), handle);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid supplier function").c_str());
        return nullptr;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Ptr || !fn->params.empty()) {
        rt_trap((std::string(api) + ": supplier must be () -> Object").c_str());
        return nullptr;
    }
    il::support::SmallVector<Slot, 1> noArgs;
    Slot result = detail::VMAccess::callFunction(vm, *fn, noArgs);
    return result.ptr;
}

/// @brief Execute a one-argument, object-returning IL callback on the VM.
/// @param vm Active VM executing the program.
/// @param callback IL function address passed as the combinator callback.
/// @param value Object payload forwarded to the callback.
/// @param api Public API name used in trap diagnostics.
/// @return Object pointer produced by the callback.
static void *runVmCallback1(VM &vm, void *callback, void *value, const char *api) {
    const il::core::Function *fn = resolveCallbackFunction(vm.module(), callback);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid callback function").c_str());
        return nullptr;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::Ptr || fn->params.size() != 1 ||
        fn->params[0].type.kind != Kind::Ptr) {
        rt_trap((std::string(api) + ": callback must be (Object) -> Object").c_str());
        return nullptr;
    }
    il::support::SmallVector<Slot, 1> callArgs;
    Slot arg{};
    arg.ptr = value;
    callArgs.push_back(arg);
    Slot result = detail::VMAccess::callFunction(vm, *fn, callArgs);
    return result.ptr;
}

/// @brief Execute a one-argument boolean predicate IL callback on the VM.
/// @param vm Active VM executing the program.
/// @param callback IL function address passed as the predicate.
/// @param value Object payload forwarded to the predicate.
/// @param api Public API name used in trap diagnostics.
/// @return Non-zero when the predicate returned true.
static int8_t runVmPredicate(VM &vm, void *callback, void *value, const char *api) {
    const il::core::Function *fn = resolveCallbackFunction(vm.module(), callback);
    if (!fn) {
        rt_trap((std::string(api) + ": invalid predicate function").c_str());
        return 0;
    }
    using Kind = il::core::Type::Kind;
    if (fn->retType.kind != Kind::I1 || fn->params.size() != 1 ||
        fn->params[0].type.kind != Kind::Ptr) {
        rt_trap((std::string(api) + ": predicate must be (Object) -> Boolean").c_str());
        return 0;
    }
    il::support::SmallVector<Slot, 1> callArgs;
    Slot arg{};
    arg.ptr = value;
    callArgs.push_back(arg);
    Slot result = detail::VMAccess::callFunction(vm, *fn, callArgs);
    return result.i64 != 0 ? 1 : 0;
}

/// @brief Context threaded through the rt_cb_invoke* strategies for VM execution.
struct VmInvokerCtx {
    VM *vm;         ///< Active virtual machine used to execute the callback.
    const char *api; ///< Public API name included in trap diagnostics.
};

/// @brief rt_cb_invoke1 strategy executing the callback on the active VM.
/// @param ctxRaw Pointer to the associated @ref VmInvokerCtx.
/// @param fn Raw IL function address to resolve and invoke.
/// @param arg Object argument forwarded to the callback.
/// @return Object pointer returned by the callback.
static void *vmInvoke1(void *ctxRaw, void *fn, void *arg) {
    auto *ctx = static_cast<VmInvokerCtx *>(ctxRaw);
    return runVmCallback1(*ctx->vm, fn, arg, ctx->api);
}

/// @brief rt_cb_invoke0 strategy executing the callback on the active VM.
/// @param ctxRaw Pointer to the associated @ref VmInvokerCtx.
/// @param fn Raw IL supplier function address to resolve and invoke.
/// @return Object pointer returned by the supplier.
static void *vmInvoke0(void *ctxRaw, void *fn) {
    auto *ctx = static_cast<VmInvokerCtx *>(ctxRaw);
    return runVmSupplier(*ctx->vm, fn, ctx->api);
}

/// @brief rt_cb_invoke_pred strategy executing the predicate on the active VM.
/// @param ctxRaw Pointer to the associated @ref VmInvokerCtx.
/// @param fn Raw IL predicate function address to resolve and invoke.
/// @param arg Object argument forwarded to the predicate.
/// @return One when the predicate succeeds; zero otherwise.
static int8_t vmInvokePred(void *ctxRaw, void *fn, void *arg) {
    auto *ctx = static_cast<VmInvokerCtx *>(ctxRaw);
    return runVmPredicate(*ctx->vm, fn, arg, ctx->api);
}

/// @brief Run a pending handle-kind supplier so subsequent C accessors see a cached value.
/// @param lazy Lazy object about to be accessed.
/// @param api Public API name used in trap diagnostics.
static void completePendingLazy(void *lazy, const char *api) {
    VM *vm = activeVMInstance();
    if (!vm)
        return;
    if (void *handle = rt_lazy_pending_handle(lazy)) {
        void *value = runVmSupplier(*vm, handle, api);
        rt_lazy_complete_obj(lazy, value);
    }
}

/// @brief Bridge @c Zanna.Functional.Lazy.New into VM-aware lazy construction.
/// @details Validates an IL supplier against the active module and stores it as
///          a pending handle.  Without an active VM, delegates to the native
///          wrapper.
/// @param args Runtime argument-storage array; element zero contains the
///        supplier function pointer when present.
/// @param result Optional storage that receives the new lazy-object pointer.
static void functional_lazy_new_handler(void **args, void *result) {
    void *supplier = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *lazy = nullptr;
    if (VM *vm = activeVMInstance()) {
        // Validate eagerly so a bad supplier traps at construction, matching the
        // fail-fast behavior a native build gets from the C type system.
        const il::core::Function *fn = resolveCallbackFunction(vm->module(), supplier);
        if (!fn) {
            rt_trap("Lazy.New: invalid supplier function");
            return;
        }
        using Kind = il::core::Type::Kind;
        if (fn->retType.kind != Kind::Ptr || !fn->params.empty()) {
            rt_trap("Lazy.New: supplier must be () -> Object");
            return;
        }
        lazy = rt_lazy_new_handle(supplier);
    } else {
        lazy = rt_lazy_new_wrapper(supplier);
    }
    if (result)
        *reinterpret_cast<void **>(result) = lazy;
}

/// @brief Bridge object-valued @c Lazy.Get access.
/// @details Completes a pending VM supplier before reading the cached object
///          through the C lazy runtime.
/// @param args Runtime argument-storage array containing the lazy receiver.
/// @param result Optional storage that receives the cached object pointer.
static void functional_lazy_get_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    completePendingLazy(lazy, "Lazy.Get");
    void *value = rt_lazy_get(lazy);
    if (result)
        *reinterpret_cast<void **>(result) = value;
}

/// @brief Bridge string-valued @c Lazy.GetStr access.
/// @details Completes a pending VM supplier and retrieves the result using the
///          string-specific C runtime accessor.
/// @param args Runtime argument-storage array containing the lazy receiver.
/// @param result Optional storage that receives the @c rt_string value.
static void functional_lazy_get_str_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    completePendingLazy(lazy, "Lazy.GetStr");
    rt_string value = rt_lazy_get_str(lazy);
    if (result)
        *reinterpret_cast<rt_string *>(result) = value;
}

/// @brief Bridge integer-valued @c Lazy.GetI64 access.
/// @details Completes a pending VM supplier and retrieves its cached signed
///          64-bit value through the C runtime.
/// @param args Runtime argument-storage array containing the lazy receiver.
/// @param result Optional storage that receives the integer value.
static void functional_lazy_get_i64_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    completePendingLazy(lazy, "Lazy.GetI64");
    int64_t value = rt_lazy_get_i64(lazy);
    if (result)
        *reinterpret_cast<int64_t *>(result) = value;
}

/// @brief Bridge @c Lazy.Force for a possibly pending VM-backed lazy.
/// @details Completes a pending supplier before forwarding to the C runtime;
///          the bridge operation has no return value.
/// @param args Runtime argument-storage array containing the lazy receiver.
/// @param result Unused result-storage pointer supplied by the generic bridge.
static void functional_lazy_force_handler(void **args, void *result) {
    (void)result;
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    completePendingLazy(lazy, "Lazy.Force");
    rt_lazy_force(lazy);
}

/// @brief Bridge @c Lazy.Map with VM-aware callback execution.
/// @details Under an active VM, forces the source, invokes the IL callback, and
///          wraps the transformed object in a completed lazy.  Native calls are
///          delegated to the C wrapper.
/// @param args Runtime argument-storage array containing the lazy receiver and
///        mapping callback.
/// @param result Optional storage that receives the mapped lazy pointer.
static void functional_lazy_map_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *mapped = nullptr;
    VM *vm = activeVMInstance();
    if (vm && lazy && callback) {
        completePendingLazy(lazy, "Lazy.Map");
        void *value = rt_lazy_get(lazy);
        void *transformed = runVmCallback1(*vm, callback, value, "Lazy.Map");
        mapped = rt_lazy_of(transformed);
    } else if (vm) {
        // Mirror the C semantics for null receiver/callback: return the source.
        mapped = lazy;
    } else {
        mapped = rt_lazy_map_wrapper(lazy, callback);
    }
    if (result)
        *reinterpret_cast<void **>(result) = mapped;
}

/// @brief Bridge @c Lazy.AndThen with VM-aware callback execution.
/// @details Under an active VM, forces the source and treats the IL callback's
///          object result as the chained lazy.  Native calls are delegated to
///          the C flat-map wrapper.
/// @param args Runtime argument-storage array containing the lazy receiver and
///        chaining callback.
/// @param result Optional storage that receives the chained lazy pointer.
static void functional_lazy_and_then_handler(void **args, void *result) {
    void *lazy = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *chained = nullptr;
    VM *vm = activeVMInstance();
    if (vm && lazy && callback) {
        completePendingLazy(lazy, "Lazy.AndThen");
        void *value = rt_lazy_get(lazy);
        chained = runVmCallback1(*vm, callback, value, "Lazy.AndThen");
    } else if (vm) {
        // Mirror the C semantics for null receiver/callback: return the source.
        chained = lazy;
    } else {
        chained = rt_lazy_flat_map_wrapper(lazy, callback);
    }
    if (result)
        *reinterpret_cast<void **>(result) = chained;
}

/// @brief Shared body for the eight Option/Result combinator handlers.
/// @details Runs the combinator core with a VM invoker whenever a VM is active;
///          otherwise forwards the receiver and callback to the native wrapper.
/// @tparam CoreFn Callable type for the VM-aware C combinator core.
/// @tparam Strategy Callback-invocation strategy type accepted by @p core.
/// @tparam NativeFn Callable type for the native combinator wrapper.
/// @param args Runtime argument-storage array containing receiver and callback.
/// @param result Optional storage that receives the combined object pointer.
/// @param api Public API name used in VM callback diagnostics.
/// @param core VM-aware combinator implementation.
/// @param strategy Adapter used by @p core to invoke the managed callback.
/// @param native Native wrapper used when no VM is active.
template <typename CoreFn, typename Strategy, typename NativeFn>
static void dispatchCombinator(void **args,
                               void *result,
                               const char *api,
                               CoreFn core,
                               Strategy strategy,
                               NativeFn native) {
    void *receiver = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *callback = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *combined = nullptr;
    if (VM *vm = activeVMInstance()) {
        VmInvokerCtx ctx{vm, api};
        combined = core(receiver, callback, strategy, &ctx);
    } else {
        combined = native(receiver, callback);
    }
    if (result)
        *reinterpret_cast<void **>(result) = combined;
}

/// @brief Bridge @c Option.Map through the shared VM-aware combinator adapter.
/// @param args Runtime argument-storage array containing option and callback.
/// @param result Optional storage that receives the mapped option.
static void functional_option_map_handler(void **args, void *result) {
    dispatchCombinator(
        args, result, "Option.Map", &rt_option_map_invoke, &vmInvoke1, &rt_option_map_wrapper);
}

/// @brief Bridge @c Option.AndThen through the VM-aware callback adapter.
/// @param args Runtime argument-storage array containing option and callback.
/// @param result Optional storage that receives the chained option.
static void functional_option_and_then_handler(void **args, void *result) {
    dispatchCombinator(args,
                       result,
                       "Option.AndThen",
                       &rt_option_and_then_invoke,
                       &vmInvoke1,
                       &rt_option_and_then_wrapper);
}

/// @brief Bridge @c Option.OrElse through the VM-aware supplier adapter.
/// @param args Runtime argument-storage array containing option and fallback
///        supplier.
/// @param result Optional storage that receives the selected option.
static void functional_option_or_else_handler(void **args, void *result) {
    dispatchCombinator(args,
                       result,
                       "Option.OrElse",
                       &rt_option_or_else_invoke,
                       &vmInvoke0,
                       &rt_option_or_else_wrapper);
}

/// @brief Bridge @c Option.Filter through the VM-aware predicate adapter.
/// @param args Runtime argument-storage array containing option and predicate.
/// @param result Optional storage that receives the filtered option.
static void functional_option_filter_handler(void **args, void *result) {
    dispatchCombinator(args,
                       result,
                       "Option.Filter",
                       &rt_option_filter_invoke,
                       &vmInvokePred,
                       &rt_option_filter_wrapper);
}

/// @brief Bridge @c Result.Map through the shared VM-aware combinator adapter.
/// @param args Runtime argument-storage array containing result and callback.
/// @param result Optional storage that receives the mapped result object.
static void functional_result_map_handler(void **args, void *result) {
    dispatchCombinator(
        args, result, "Result.Map", &rt_result_map_invoke, &vmInvoke1, &rt_result_map_wrapper);
}

/// @brief Bridge @c Result.MapErr through the VM-aware callback adapter.
/// @param args Runtime argument-storage array containing result and error
///        mapping callback.
/// @param result Optional storage that receives the mapped result object.
static void functional_result_map_err_handler(void **args, void *result) {
    dispatchCombinator(args,
                       result,
                       "Result.MapErr",
                       &rt_result_map_err_invoke,
                       &vmInvoke1,
                       &rt_result_map_err_wrapper);
}

/// @brief Bridge @c Result.AndThen through the VM-aware callback adapter.
/// @param args Runtime argument-storage array containing result and chaining
///        callback.
/// @param result Optional storage that receives the chained result object.
static void functional_result_and_then_handler(void **args, void *result) {
    dispatchCombinator(args,
                       result,
                       "Result.AndThen",
                       &rt_result_and_then_invoke,
                       &vmInvoke1,
                       &rt_result_and_then_wrapper);
}

/// @brief Bridge @c Result.OrElse through the VM-aware callback adapter.
/// @param args Runtime argument-storage array containing result and recovery
///        callback.
/// @param result Optional storage that receives the recovered result object.
static void functional_result_or_else_handler(void **args, void *result) {
    dispatchCombinator(args,
                       result,
                       "Result.OrElse",
                       &rt_result_or_else_invoke,
                       &vmInvoke1,
                       &rt_result_or_else_wrapper);
}

} // namespace

/// @brief Register VM-aware Zanna.Functional externals with the runtime bridge.
/// @details Installs the `Zanna.Functional.Lazy` handlers so deferred suppliers
///          and combinator callbacks execute through the active VM instead of
///          being invoked as raw C function pointers.
void registerFunctionalRuntimeExternals() {
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.New";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_new_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.Get";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_get_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.GetStr";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::Str});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_get_str_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.GetI64";
        ext.signature = make_signature(ext.name, {SigParam::Ptr}, {SigParam::I64});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_get_i64_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.Force";
        ext.signature = make_signature(ext.name, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_force_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.Map";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_map_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Functional.Lazy.AndThen";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&functional_lazy_and_then_handler);
        RuntimeBridge::registerExtern(ext);
    }

    struct CombinatorExtern {
        const char *name;                 ///< Public runtime external name.
        /// @brief Generic bridge handler function.
        /// @param args Array of pointers to argument storage.
        /// @param result Pointer to result storage.
        void (*handler)(void **, void *);
    };
    static constexpr CombinatorExtern kCombinators[] = {
        {"Zanna.Option.Map", &functional_option_map_handler},
        {"Zanna.Option.AndThen", &functional_option_and_then_handler},
        {"Zanna.Option.OrElse", &functional_option_or_else_handler},
        {"Zanna.Option.Filter", &functional_option_filter_handler},
        {"Zanna.Result.Map", &functional_result_map_handler},
        {"Zanna.Result.MapErr", &functional_result_map_err_handler},
        {"Zanna.Result.AndThen", &functional_result_and_then_handler},
        {"Zanna.Result.OrElse", &functional_result_or_else_handler},
    };
    for (const auto &entry : kCombinators) {
        ExternDesc ext;
        ext.name = entry.name;
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Ptr}, {SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(entry.handler);
        RuntimeBridge::registerExtern(ext);
    }
}

} // namespace il::vm
