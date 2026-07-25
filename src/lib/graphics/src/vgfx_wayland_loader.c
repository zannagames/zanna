//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_loader.c
// Purpose: Resolve the Wayland client ABI without a link-time product dependency.
// Key invariants:
//   - Missing symbols fail atomically with the first missing symbol named.
//   - Failed and closed tables contain no dangling function or interface pointers.
// Ownership/Lifetime:
//   - dlopen handles are owned exclusively by one dispatch table.
//   - dlerror text is copied before the loader state can change.
// Links: src/lib/graphics/src/vgfx_wayland_loader.h,
//        docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#include "vgfx_wayland_loader.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/// @file
/// @brief Implements atomic runtime loading of the Wayland client ABI.
/// @details The loader resolves both callable entry points and exported protocol-interface
/// descriptors. It deliberately copies function addresses through `memcpy` so the dynamic
/// loader's object pointers are transferred without a direct object-to-function cast.

/// @brief Write a stable backend-unavailable diagnostic into a caller buffer.
/// @param error Destination buffer, or NULL when no diagnostic is requested.
/// @param error_size Capacity of @p error in bytes, including its terminator.
/// @param detail Loader-specific explanation, or NULL for the generic fallback.
static void vgfx_wayland_loader_error(char *error, uint32_t error_size, const char *detail) {
    if (!error || error_size == 0)
        return;
    (void)snprintf(error,
                   (size_t)error_size,
                   "Wayland backend unavailable: %s",
                   detail ? detail : "unknown loader error");
}

/// @copydoc vgfx_wayland_loader_close
void vgfx_wayland_loader_close(vgfx_wayland_client_api_t *api) {
    if (!api)
        return;
    if (api->library)
        dlclose(api->library);
    memset(api, 0, sizeof(*api));
}

/// @copydoc vgfx_wayland_loader_open
int vgfx_wayland_loader_open(vgfx_wayland_client_api_t *api,
                             const char *library_name,
                             char *error,
                             uint32_t error_size) {
    const char *names[] = {"libwayland-client.so.0", "libwayland-client.so"};
    const char *loader_error = NULL;
    const char *missing = NULL;
    if (error && error_size > 0)
        error[0] = '\0';
    if (!api) {
        vgfx_wayland_loader_error(error, error_size, "loader destination is null");
        return 0;
    }
    memset(api, 0, sizeof(*api));

    if (library_name) {
        api->library = dlopen(library_name, RTLD_NOW | RTLD_LOCAL);
    } else {
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !api->library; ++i)
            api->library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (!api->library) {
        loader_error = dlerror();
        vgfx_wayland_loader_error(
            error, error_size, loader_error ? loader_error : "client library was not found");
        return 0;
    }

/// @brief Resolve one required callable entry point into the dispatch table.
/// @details Failure records the missing symbol and transfers control to the common atomic
/// cleanup path. `memcpy` preserves the function-pointer representation accepted by the ABI.
#define VGFX_WL_LOAD_FIELD(field, symbol)                                                          \
    do {                                                                                            \
        void *address = dlsym(api->library, symbol);                                                \
        if (!address) {                                                                             \
            missing = symbol;                                                                       \
            goto fail;                                                                              \
        }                                                                                           \
        memcpy(&api->field, &address, sizeof(address));                                             \
    } while (0)

/// @brief Resolve one required exported protocol-interface descriptor.
/// @details Failure records the missing symbol and transfers control to the common atomic
/// cleanup path.
#define VGFX_WL_LOAD_INTERFACE(field, symbol)                                                       \
    do {                                                                                            \
        api->field = (const struct wl_interface *)dlsym(api->library, symbol);                       \
        if (!api->field) {                                                                          \
            missing = symbol;                                                                       \
            goto fail;                                                                              \
        }                                                                                           \
    } while (0)

    VGFX_WL_LOAD_FIELD(display_connect, "wl_display_connect");
    VGFX_WL_LOAD_FIELD(display_disconnect, "wl_display_disconnect");
    VGFX_WL_LOAD_FIELD(display_dispatch, "wl_display_dispatch");
    VGFX_WL_LOAD_FIELD(display_dispatch_pending, "wl_display_dispatch_pending");
    VGFX_WL_LOAD_FIELD(display_roundtrip, "wl_display_roundtrip");
    VGFX_WL_LOAD_FIELD(display_flush, "wl_display_flush");
    VGFX_WL_LOAD_FIELD(display_get_fd, "wl_display_get_fd");
    VGFX_WL_LOAD_FIELD(display_prepare_read, "wl_display_prepare_read");
    VGFX_WL_LOAD_FIELD(display_read_events, "wl_display_read_events");
    VGFX_WL_LOAD_FIELD(display_cancel_read, "wl_display_cancel_read");
    VGFX_WL_LOAD_FIELD(display_get_error, "wl_display_get_error");
    VGFX_WL_LOAD_FIELD(proxy_destroy, "wl_proxy_destroy");
    VGFX_WL_LOAD_FIELD(proxy_add_listener, "wl_proxy_add_listener");
    VGFX_WL_LOAD_FIELD(proxy_marshal_flags, "wl_proxy_marshal_flags");
    VGFX_WL_LOAD_FIELD(proxy_get_version, "wl_proxy_get_version");
    VGFX_WL_LOAD_FIELD(log_set_handler_client, "wl_log_set_handler_client");

    VGFX_WL_LOAD_INTERFACE(display_interface, "wl_display_interface");
    VGFX_WL_LOAD_INTERFACE(registry_interface, "wl_registry_interface");
    VGFX_WL_LOAD_INTERFACE(callback_interface, "wl_callback_interface");
    VGFX_WL_LOAD_INTERFACE(compositor_interface, "wl_compositor_interface");
    VGFX_WL_LOAD_INTERFACE(subcompositor_interface, "wl_subcompositor_interface");
    VGFX_WL_LOAD_INTERFACE(subsurface_interface, "wl_subsurface_interface");
    VGFX_WL_LOAD_INTERFACE(surface_interface, "wl_surface_interface");
    VGFX_WL_LOAD_INTERFACE(region_interface, "wl_region_interface");
    VGFX_WL_LOAD_INTERFACE(shm_interface, "wl_shm_interface");
    VGFX_WL_LOAD_INTERFACE(shm_pool_interface, "wl_shm_pool_interface");
    VGFX_WL_LOAD_INTERFACE(buffer_interface, "wl_buffer_interface");
    VGFX_WL_LOAD_INTERFACE(seat_interface, "wl_seat_interface");
    VGFX_WL_LOAD_INTERFACE(pointer_interface, "wl_pointer_interface");
    VGFX_WL_LOAD_INTERFACE(keyboard_interface, "wl_keyboard_interface");
    VGFX_WL_LOAD_INTERFACE(touch_interface, "wl_touch_interface");
    VGFX_WL_LOAD_INTERFACE(output_interface, "wl_output_interface");
    VGFX_WL_LOAD_INTERFACE(data_device_manager_interface, "wl_data_device_manager_interface");
    VGFX_WL_LOAD_INTERFACE(data_device_interface, "wl_data_device_interface");
    VGFX_WL_LOAD_INTERFACE(data_source_interface, "wl_data_source_interface");
    VGFX_WL_LOAD_INTERFACE(data_offer_interface, "wl_data_offer_interface");

#undef VGFX_WL_LOAD_INTERFACE
#undef VGFX_WL_LOAD_FIELD
    return 1;

fail: {
    char detail[192];
    (void)snprintf(detail, sizeof(detail), "client library is missing symbol %s", missing);
    vgfx_wayland_loader_close(api);
    vgfx_wayland_loader_error(error, error_size, detail);
    return 0;
}
}
