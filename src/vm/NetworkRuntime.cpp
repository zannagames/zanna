//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/NetworkRuntime.cpp
// Purpose: VM-aware runtime helpers for Zanna.Network.HttpServer handler binding.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements VM-aware HTTP and HTTPS server handler binding.
/// @details Interpreted handler addresses are resolved against the active module
///          and wrapped in an owned dispatch payload.  Each native request
///          creates a VM sharing the parent program state and external registry,
///          while native callers retain the direct C callback path.

#include "vm/RuntimeBridge.hpp"

#include "il/core/Module.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "rt.hpp"
#include "rt_http_server.h"
#include "rt_https_server.h"
#include "rt_string.h"
#include "support/small_vector.hpp"

#include "vm/OpHandlerAccess.hpp"
#include "vm/VM.hpp"
#include "vm/VMContext.hpp"
#include <exception>
#include <string>

namespace il::vm {
namespace {

using il::runtime::signatures::make_signature;
using il::runtime::signatures::SigParam;

/// @brief Retained state required to invoke an interpreted HTTP handler later.
/// @details The native server owns this heap payload after binding and releases
///          it through @ref vm_http_handler_payload_destroy.
struct VmHttpHandlerPayload {
    const il::core::Module *module = nullptr; ///< Module owning @ref entry.
    std::shared_ptr<VM::ProgramState> program; ///< Shared mutable program state.
    ExternRegistry *externRegistry = nullptr; ///< Retained external registry.
    const il::core::Function *entry = nullptr; ///< Resolved request handler.
};

/// @brief Destroy a native server's retained VM handler payload.
/// @param raw Opaque pointer previously allocated during handler binding; may be
///        @c nullptr.
extern "C" void vm_http_handler_payload_destroy(void *raw) {
    auto *payload = static_cast<VmHttpHandlerPayload *>(raw);
    if (!payload)
        return;
    releaseExternRegistry(payload->externRegistry);
    delete payload;
}

/// @brief Dispatch one native HTTP request to an interpreted handler.
/// @details Creates a VM sharing the retained program and external registry,
///          passes request and response pointers as two VM arguments, and turns
///          traps or exceptions into fatal runtime diagnostics at the C boundary.
/// @param raw Opaque @ref VmHttpHandlerPayload owned by the native server.
/// @param req Native request object forwarded to managed code.
/// @param res Native response object forwarded to managed code.
extern "C" void vm_http_handler_dispatch(void *raw, void *req, void *res) {
    auto *payload = static_cast<VmHttpHandlerPayload *>(raw);
    if (!payload || !payload->module || !payload->entry)
        rt_abort("HttpServer.BindHandler: invalid handler payload");

    try {
        VM vm(*payload->module, payload->program);
        vm.setExternRegistry(payload->externRegistry);
        il::support::SmallVector<Slot, 2> args;
        Slot reqSlot{};
        reqSlot.ptr = req;
        args.push_back(reqSlot);
        Slot resSlot{};
        resSlot.ptr = res;
        args.push_back(resSlot);
        detail::VMAccess::callFunction(vm, *payload->entry, args);
    } catch (const RuntimeTrapSignal &signal) {
        const std::string message =
            signal.message.empty() ? "HttpServer.BindHandler: trapped VM handler" : signal.message;
        rt_abort(message.c_str());
    } catch (const std::exception &ex) {
        const std::string message =
            std::string("HttpServer.BindHandler: unhandled exception: ") + ex.what();
        rt_abort(message.c_str());
    } catch (...) {
        rt_abort("HttpServer.BindHandler: unhandled non-standard exception");
    }
}

/// @brief Resolve a raw handler address against a module's function objects.
/// @param module Module whose function storage is searched.
/// @param entry Candidate address supplied through the runtime ABI.
/// @return Matching function pointer, or @c nullptr for a null or foreign entry.
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

/// @brief Require an HTTP handler signature of two pointers returning unit.
/// @param fn Resolved IL function to validate.
static void validateHttpHandlerSignature(const il::core::Function &fn) {
    using Kind = il::core::Type::Kind;
    if (fn.retType.kind != Kind::Void || fn.params.size() != 2 ||
        fn.params[0].type.kind != Kind::Ptr || fn.params[1].type.kind != Kind::Ptr) {
        rt_trap("HttpServer.BindHandler: invalid handler signature");
    }
}

/// @brief Native server function that binds a direct callback.
/// @param server Native server handle.
/// @param route Runtime route string.
/// @param handler Direct callback pointer.
using NativeBindHandlerFn = void (*)(void *, rt_string, void *);
/// @brief Native server function that binds a callback plus opaque payload.
/// @param server Native server handle.
/// @param route Runtime route string.
/// @param handler Dispatch callback pointer.
/// @param payload Opaque callback payload.
/// @param cleanup Payload cleanup callback.
using DispatchBindHandlerFn = void (*)(void *, rt_string, void *, void *, void *);

/// @brief Shared HTTP/HTTPS handler-binding bridge.
/// @details Native execution forwards the callback unchanged.  Under an active
///          VM, the function validates the IL entry, retains shared execution
///          state, and binds the C dispatch and cleanup trampolines.
/// @param args Runtime argument-storage array containing server, route tag, and
///        handler entry.
/// @param result Unused result-storage pointer for this unit-returning API.
/// @param trap_name Diagnostic used when the entry argument is null.
/// @param native_bind Direct native binding implementation.
/// @param dispatch_bind Payload-aware native binding implementation.
static void network_server_bind_handler_handler(void **args,
                                                void *result,
                                                const char *trap_name,
                                                NativeBindHandlerFn native_bind,
                                                DispatchBindHandlerFn dispatch_bind) {
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

    if (!entry)
        rt_trap(trap_name);

    VM *parentVm = activeVMInstance();
    if (!parentVm) {
        native_bind(server, tag, entry);
        return;
    }

    std::shared_ptr<VM::ProgramState> program = parentVm->programState();
    if (!program)
        rt_trap("HttpServer.BindHandler: invalid runtime state");

    const il::core::Module &module = parentVm->module();
    const il::core::Function *entryFn = resolveEntryFunction(module, entry);
    if (!entryFn)
        rt_trap("HttpServer.BindHandler: invalid entry");
    validateHttpHandlerSignature(*entryFn);

    auto *payload =
        new VmHttpHandlerPayload{&module, std::move(program), parentVm->externRegistry(), entryFn};
    retainExternRegistry(payload->externRegistry);
    dispatch_bind(server,
                  tag,
                  reinterpret_cast<void *>(&vm_http_handler_dispatch),
                  payload,
                  reinterpret_cast<void *>(&vm_http_handler_payload_destroy));
}

/// @brief Bind a handler to a plain HTTP server.
/// @param args Runtime argument-storage array forwarded to the shared binder.
/// @param result Unused result-storage pointer forwarded to the shared binder.
static void network_http_server_bind_handler_handler(void **args, void *result) {
    network_server_bind_handler_handler(args,
                                        result,
                                        "HttpServer.BindHandler: null entry",
                                        &rt_http_server_bind_handler,
                                        &rt_http_server_bind_handler_dispatch);
}

/// @brief Bind a handler to an HTTPS server.
/// @param args Runtime argument-storage array forwarded to the shared binder.
/// @param result Unused result-storage pointer forwarded to the shared binder.
static void network_https_server_bind_handler_handler(void **args, void *result) {
    network_server_bind_handler_handler(args,
                                        result,
                                        "HttpsServer.BindHandler: null entry",
                                        &rt_https_server_bind_handler,
                                        &rt_https_server_bind_handler_dispatch);
}

} // namespace

/// @brief Register VM-aware HTTP and HTTPS handler-binding externals.
/// @details Both public APIs share the same three-argument signature and select
///          protocol-specific native bind functions behind their bridge handlers.
void registerNetworkRuntimeExternals() {
    {
        ExternDesc ext;
        ext.name = "Zanna.Network.HttpServer.BindHandler";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Str, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&network_http_server_bind_handler_handler);
        RuntimeBridge::registerExtern(ext);
    }
    {
        ExternDesc ext;
        ext.name = "Zanna.Network.HttpsServer.BindHandler";
        ext.signature = make_signature(ext.name, {SigParam::Ptr, SigParam::Str, SigParam::Ptr});
        ext.fn = reinterpret_cast<void *>(&network_https_server_bind_handler_handler);
        RuntimeBridge::registerExtern(ext);
    }
}

} // namespace il::vm
