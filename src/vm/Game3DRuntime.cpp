//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/Game3DRuntime.cpp
// Purpose: VM-aware Game3D callback-loop bridges for interpreted Zanna code.
// Key invariants:
//   - Script callback references are resolved against the active VM module.
//   - Runtime receives only native C-callable trampoline pointers.
// Ownership/Lifetime:
//   - Callback scopes are thread-local and valid only during synchronous Game3D calls.
//   - The VM owns program state and callback functions; this bridge only borrows them.
// Links: src/runtime/graphics/3d/rt_game3d.h, src/il/runtime/runtime.def
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief VM-aware runtime helpers for Game3D callback loops.
/// @details Resolves interpreted update and overlay functions, validates their
///          signatures, installs thread-local trampoline state for synchronous
///          native loops, and translates callback failures into runtime traps.

#include "vm/RuntimeBridge.hpp"

#include "il/core/Module.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "rt.hpp"
#include "rt_game3d.h"
#include "rt_platform.h"
#include "support/small_vector.hpp"
#include "vm/OpHandlerAccess.hpp"
#include "vm/VM.hpp"
#include "vm/VMContext.hpp"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <string>

namespace il::vm {
namespace {

using il::runtime::signatures::make_signature;
using il::runtime::signatures::SigParam;

/// @brief Thread-local callback state visible to the native Game3D trampolines.
/// @details Scopes form a linked stack so nested synchronous runtime calls
///          restore the callback context that was active before entry.
struct VmGame3DCallbackScope {
    VM *vm = nullptr; ///< Active VM that owns the callback functions.
    const il::core::Function *update = nullptr; ///< Per-frame update callback.
    const il::core::Function *overlay = nullptr; ///< Overlay drawing callback.
    VmGame3DCallbackScope *previous = nullptr; ///< Enclosing callback scope.
};

/// @brief Callback scope currently installed on this runtime thread.
thread_local VmGame3DCallbackScope *tlsGame3DScope = nullptr;

/// @brief Resolve a raw callback address against a module's function storage.
/// @param module Module whose function objects are searched.
/// @param entry Candidate pointer supplied through the runtime ABI.
/// @return Pointer to the matching function, or @c nullptr for a null or
///         foreign address.
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

/// @brief Require the Game3D update callback signature @c (Float)->Unit.
/// @param fn Resolved IL callback to validate.
/// @param api Public API name prefixed to trap diagnostics.
static void validateUpdateSignature(const il::core::Function &fn, const char *api) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind == Kind::Void && fn.params.size() == 1 &&
        fn.params[0].type.kind == Kind::F64) {
        return;
    }
    std::string message(api);
    message += ": update callback must have signature (Float) -> Unit";
    rt_trap(message.c_str());
}

/// @brief Require the Game3D overlay callback signature @c ()->Unit.
/// @param fn Resolved IL callback to validate.
/// @param api Public API name prefixed to trap diagnostics.
static void validateOverlaySignature(const il::core::Function &fn, const char *api) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind == Kind::Void && fn.params.empty())
        return;
    std::string message(api);
    message += ": overlay callback must have signature () -> Unit";
    rt_trap(message.c_str());
}

/// @brief Invoke the update function stored in a callback scope.
/// @param scope Active scope containing a VM and resolved update function.
/// @param dt Frame delta forwarded as the callback's floating-point argument.
static void invokeVmUpdate(VmGame3DCallbackScope *scope, double dt) {
    if (!scope || !scope->vm || !scope->update) {
        rt_trap("Game3D VM callback bridge: invalid update callback scope");
        return;
    }

    il::support::SmallVector<Slot, 1> args;
    Slot dtSlot{};
    dtSlot.f64 = dt;
    args.push_back(dtSlot);
    detail::VMAccess::callFunction(*scope->vm, *scope->update, args);
}

/// @brief Invoke the zero-argument overlay function stored in a callback scope.
/// @param scope Active scope containing a VM and resolved overlay function.
static void invokeVmOverlay(VmGame3DCallbackScope *scope) {
    if (!scope || !scope->vm || !scope->overlay) {
        rt_trap("Game3D VM callback bridge: invalid overlay callback scope");
        return;
    }

    il::support::SmallVector<Slot, 1> args;
    detail::VMAccess::callFunction(*scope->vm, *scope->overlay, args);
}

/// @brief Native C trampoline that re-enters the active VM update callback.
/// @details Converts VM traps and arbitrary C++ exceptions into C runtime traps
///          so no exception crosses the native Game3D callback boundary.
/// @param dt Frame delta supplied by the native loop.
extern "C" void vm_game3d_update_trampoline(double dt) {
    VmGame3DCallbackScope *scope = tlsGame3DScope;
    try {
        invokeVmUpdate(scope, dt);
    } catch (const RuntimeTrapSignal &signal) {
        rt_trap(signal.message.empty() ? "Game3D.World3D: trapped VM update callback"
                                       : signal.message.c_str());
    } catch (const std::exception &ex) {
        const std::string message =
            std::string("Game3D.World3D: unhandled VM update exception: ") + ex.what();
        rt_trap(message.c_str());
    } catch (...) {
        rt_trap("Game3D.World3D: unhandled VM update exception");
    }
}

/// @brief Native C trampoline that re-enters the active VM overlay callback.
/// @details Uses the thread-local callback scope and converts all callback
///          failures into C runtime traps.
extern "C" void vm_game3d_overlay_trampoline(void) {
    VmGame3DCallbackScope *scope = tlsGame3DScope;
    try {
        invokeVmOverlay(scope);
    } catch (const RuntimeTrapSignal &signal) {
        rt_trap(signal.message.empty() ? "Game3D.World3D: trapped VM overlay callback"
                                       : signal.message.c_str());
    } catch (const std::exception &ex) {
        const std::string message =
            std::string("Game3D.World3D: unhandled VM overlay exception: ") + ex.what();
        rt_trap(message.c_str());
    } catch (...) {
        rt_trap("Game3D.World3D: unhandled VM overlay exception");
    }
}

/// @brief Run a native Game3D operation with an installed VM callback scope.
/// @details Pushes @p scope into thread-local state, installs a @c setjmp-based
///          runtime-trap recovery point, invokes @p fn synchronously, restores
///          prior state, and re-raises any captured trap after cleanup.
/// @tparam Fn Nullary callable type for the native Game3D operation.
/// @param scope Callback scope exposed to update and overlay trampolines.
/// @param fn Native loop or overlay operation to execute.
template <typename Fn>
static void invokeGame3DLoopWithScope(VmGame3DCallbackScope &scope, Fn &&fn) {
    char trapMessage[512] = "";
    int trapped = 0;
    jmp_buf recovery;

    scope.previous = tlsGame3DScope;
    tlsGame3DScope = &scope;
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
    tlsGame3DScope = scope.previous;
    if (trapped)
        rt_trap(trapMessage);
}

/// @brief Query whether execution currently occurs inside a VM.
/// @return Active VM instance, or @c nullptr for a native runtime call.
static VM *activeVmOrNative() {
    return activeVMInstance();
}

/// @brief Resolve a callback in the active VM module or report a runtime trap.
/// @param vm Active VM whose module owns valid callbacks.
/// @param entry Raw callback address received through the runtime ABI.
/// @param api Public API name prefixed to trap diagnostics.
/// @return Resolved module function; trap handling is invoked when resolution
///         fails.
static const il::core::Function *resolveVmCallback(VM &vm, void *entry, const char *api) {
    const il::core::Function *fn = resolveEntryFunction(vm.module(), entry);
    if (!fn) {
        std::string message(api);
        message += ": callback is not a function in the active VM module";
        rt_trap(message.c_str());
    }
    return fn;
}

/// @brief Bridge the variable-step @c World3D.Run loop.
/// @details Interpreted callbacks are resolved and routed through the update
///          trampoline; native calls retain the original function pointer.
/// @param args Runtime argument-storage array containing world and update
///        callback pointers.
/// @param result Unused result-storage pointer for this unit-returning API.
static void game3d_run_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *update = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;

    if (VM *vm = activeVmOrNative()) {
        const il::core::Function *updateFn = resolveVmCallback(*vm, update, "Game3D.World3D.run");
        validateUpdateSignature(*updateFn, "Game3D.World3D.run");
        VmGame3DCallbackScope scope{vm, updateFn, nullptr, nullptr};
        /// @brief Enter the native variable-step loop with the VM update trampoline.
        invokeGame3DLoopWithScope(scope, [&]() {
            rt_game3d_world_run(world, reinterpret_cast<void *>(&vm_game3d_update_trampoline));
        });
        return;
    }

    rt_game3d_world_run(world, update);
}

/// @brief Bridge the variable-step world loop with an overlay callback.
/// @details Validates both interpreted callbacks and exposes them through a
///          shared scope for the native update and overlay trampolines.
/// @param args Runtime argument-storage array containing world, update, and
///        overlay pointers.
/// @param result Unused result-storage pointer for this unit-returning API.
static void game3d_run_with_overlay_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *update = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;
    void *overlay = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;

    if (VM *vm = activeVmOrNative()) {
        const il::core::Function *updateFn =
            resolveVmCallback(*vm, update, "Game3D.World3D.runWithOverlay");
        const il::core::Function *overlayFn =
            resolveVmCallback(*vm, overlay, "Game3D.World3D.runWithOverlay");
        validateUpdateSignature(*updateFn, "Game3D.World3D.runWithOverlay");
        validateOverlaySignature(*overlayFn, "Game3D.World3D.runWithOverlay");
        VmGame3DCallbackScope scope{vm, updateFn, overlayFn, nullptr};
        /// @brief Enter the native variable-step loop with VM update and overlay trampolines.
        invokeGame3DLoopWithScope(scope, [&]() {
            rt_game3d_world_run_with_overlay(
                world,
                reinterpret_cast<void *>(&vm_game3d_update_trampoline),
                reinterpret_cast<void *>(&vm_game3d_overlay_trampoline));
        });
        return;
    }

    rt_game3d_world_run_with_overlay(world, update, overlay);
}

/// @brief Bridge a fixed-step @c World3D.RunFixed loop.
/// @param args Runtime argument-storage array containing world pointer, fixed
///        time step, and update callback.
/// @param result Unused result-storage pointer for this unit-returning API.
static void game3d_run_fixed_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    double step = args && args[1] ? *reinterpret_cast<double *>(args[1]) : 0.0;
    void *update = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;

    if (VM *vm = activeVmOrNative()) {
        const il::core::Function *updateFn =
            resolveVmCallback(*vm, update, "Game3D.World3D.runFixed");
        validateUpdateSignature(*updateFn, "Game3D.World3D.runFixed");
        VmGame3DCallbackScope scope{vm, updateFn, nullptr, nullptr};
        /// @brief Enter the native fixed-step loop with the VM update trampoline.
        invokeGame3DLoopWithScope(scope, [&]() {
            rt_game3d_world_run_fixed(
                world, step, reinterpret_cast<void *>(&vm_game3d_update_trampoline));
        });
        return;
    }

    rt_game3d_world_run_fixed(world, step, update);
}

/// @brief Bridge a fixed-step world loop with an overlay callback.
/// @details Installs validated VM callbacks for interpreted execution or passes
///          native callback pointers directly to the C runtime.
/// @param args Runtime argument-storage array containing world, time step,
///        update callback, and overlay callback.
/// @param result Unused result-storage pointer for this unit-returning API.
static void game3d_run_fixed_with_overlay_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    double step = args && args[1] ? *reinterpret_cast<double *>(args[1]) : 0.0;
    void *update = args && args[2] ? *reinterpret_cast<void **>(args[2]) : nullptr;
    void *overlay = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;

    if (VM *vm = activeVmOrNative()) {
        const il::core::Function *updateFn =
            resolveVmCallback(*vm, update, "Game3D.World3D.runFixedWithOverlay");
        const il::core::Function *overlayFn =
            resolveVmCallback(*vm, overlay, "Game3D.World3D.runFixedWithOverlay");
        validateUpdateSignature(*updateFn, "Game3D.World3D.runFixedWithOverlay");
        validateOverlaySignature(*overlayFn, "Game3D.World3D.runFixedWithOverlay");
        VmGame3DCallbackScope scope{vm, updateFn, overlayFn, nullptr};
        /// @brief Enter the native fixed-step loop with VM update and overlay trampolines.
        invokeGame3DLoopWithScope(scope, [&]() {
            rt_game3d_world_run_fixed_with_overlay(
                world,
                step,
                reinterpret_cast<void *>(&vm_game3d_update_trampoline),
                reinterpret_cast<void *>(&vm_game3d_overlay_trampoline));
        });
        return;
    }

    rt_game3d_world_run_fixed_with_overlay(world, step, update, overlay);
}

/// @brief Bridge a fixed-count, fixed-step @c World3D.RunFrames loop.
/// @param args Runtime argument-storage array containing world, frame count,
///        time step, and update callback.
/// @param result Unused result-storage pointer for this unit-returning API.
static void game3d_run_frames_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    int64_t frames = args && args[1] ? *reinterpret_cast<int64_t *>(args[1]) : 0;
    double step = args && args[2] ? *reinterpret_cast<double *>(args[2]) : 0.0;
    void *update = args && args[3] ? *reinterpret_cast<void **>(args[3]) : nullptr;

    if (VM *vm = activeVmOrNative()) {
        const il::core::Function *updateFn =
            resolveVmCallback(*vm, update, "Game3D.World3D.runFrames");
        validateUpdateSignature(*updateFn, "Game3D.World3D.runFrames");
        VmGame3DCallbackScope scope{vm, updateFn, nullptr, nullptr};
        /// @brief Enter the native fixed-count loop with the VM update trampoline.
        invokeGame3DLoopWithScope(scope, [&]() {
            rt_game3d_world_run_frames(
                world, frames, step, reinterpret_cast<void *>(&vm_game3d_update_trampoline));
        });
        return;
    }

    rt_game3d_world_run_frames(world, frames, step, update);
}

/// @brief Bridge a one-shot @c World3D.DrawOverlay callback.
/// @details Uses the overlay trampoline for an interpreted callback and direct
///          forwarding when invoked outside an active VM.
/// @param args Runtime argument-storage array containing world and overlay
///        callback pointers.
/// @param result Unused result-storage pointer for this unit-returning API.
static void game3d_draw_overlay_handler(void **args, void *result) {
    (void)result;
    void *world = args && args[0] ? *reinterpret_cast<void **>(args[0]) : nullptr;
    void *overlay = args && args[1] ? *reinterpret_cast<void **>(args[1]) : nullptr;

    if (VM *vm = activeVmOrNative()) {
        const il::core::Function *overlayFn =
            resolveVmCallback(*vm, overlay, "Game3D.World3D.drawOverlay");
        validateOverlaySignature(*overlayFn, "Game3D.World3D.drawOverlay");
        VmGame3DCallbackScope scope{vm, nullptr, overlayFn, nullptr};
        /// @brief Invoke one native overlay draw through the VM overlay trampoline.
        invokeGame3DLoopWithScope(scope, [&]() {
            rt_game3d_world_draw_overlay(world,
                                         reinterpret_cast<void *>(&vm_game3d_overlay_trampoline));
        });
        return;
    }

    rt_game3d_world_draw_overlay(world, overlay);
}

/// @brief Register a unit-returning Game3D bridge handler.
/// @param name Fully qualified runtime external name.
/// @param params Ordered ABI parameter kinds for the external.
/// @param handler Generic bridge-handler address.
static void registerExtern(const char *name,
                           std::initializer_list<SigParam::Kind> params,
                           void *handler) {
    ExternDesc ext;
    ext.name = name;
    ext.signature = make_signature(ext.name, params);
    ext.fn = handler;
    RuntimeBridge::registerExtern(ext);
}

} // namespace

/// @brief Register all VM-aware Game3D loop and overlay externals.
/// @details Publishes the six bridge handlers with signatures matching the
///          runtime registry so managed callbacks never reach native code as raw
///          IL function addresses.
void registerGame3DRuntimeExternals() {
    registerExtern("Zanna.Game3D.World3D.Run",
                   {SigParam::Ptr, SigParam::Ptr},
                   reinterpret_cast<void *>(&game3d_run_handler));
    registerExtern("Zanna.Game3D.World3D.RunWithOverlay",
                   {SigParam::Ptr, SigParam::Ptr, SigParam::Ptr},
                   reinterpret_cast<void *>(&game3d_run_with_overlay_handler));
    registerExtern("Zanna.Game3D.World3D.RunFixed",
                   {SigParam::Ptr, SigParam::F64, SigParam::Ptr},
                   reinterpret_cast<void *>(&game3d_run_fixed_handler));
    registerExtern("Zanna.Game3D.World3D.RunFixedWithOverlay",
                   {SigParam::Ptr, SigParam::F64, SigParam::Ptr, SigParam::Ptr},
                   reinterpret_cast<void *>(&game3d_run_fixed_with_overlay_handler));
    registerExtern("Zanna.Game3D.World3D.RunFrames",
                   {SigParam::Ptr, SigParam::I64, SigParam::F64, SigParam::Ptr},
                   reinterpret_cast<void *>(&game3d_run_frames_handler));
    registerExtern("Zanna.Game3D.World3D.DrawOverlay",
                   {SigParam::Ptr, SigParam::Ptr},
                   reinterpret_cast<void *>(&game3d_draw_overlay_handler));
}

} // namespace il::vm
