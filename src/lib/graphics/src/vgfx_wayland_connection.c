//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_connection.c
// Purpose: Establish a dynamically loaded Wayland connection and inventory globals.
// Key invariants:
//   - Registry versions are recorded exactly as advertised and capped when bound later.
//   - Required-global failures identify the first missing stable interface.
// Ownership/Lifetime:
//   - Listener callbacks borrow interface strings only for callback duration.
//   - Close releases registry, display, then dynamic-library state in that order.
// Links: src/lib/graphics/src/vgfx_wayland_connection.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Wayland dynamic connection and registry-discovery implementation.

#include "vgfx_wayland_connection.h"

#include <stdio.h>
#include <string.h>

enum { WL_DISPLAY_GET_REGISTRY = 1 };

/// @brief Local ABI for Wayland registry add/remove listener callbacks.
typedef struct vgfx_wl_registry_listener {
    void (*global)(void *data,
                   struct wl_registry *registry,
                   uint32_t name,
                   const char *interface,
                   uint32_t version);
    void (*global_remove)(void *data, struct wl_registry *registry, uint32_t name);
} vgfx_wl_registry_listener_t;

/// @brief Format a stable Wayland connection failure diagnostic.
/// @param error Destination buffer, or NULL.
/// @param size Destination capacity including the terminator.
/// @param detail Failure-specific detail, or NULL for a generic phrase.
static void vgfx_wayland_connection_error(char *error, uint32_t size, const char *detail) {
    if (!error || size == 0)
        return;
    (void)snprintf(error,
                   (size_t)size,
                   "Wayland backend unavailable: %s",
                   detail ? detail : "unknown connection error");
}

/// @brief Inventory one advertised registry global.
/// @details Records known singleton name/version pairs and capability bits,
///          appends up to sixteen output entries, and then notifies the borrowed
///          observer while the interface string remains valid.
/// @param data Borrowed `vgfx_wayland_connection_t` listener context.
/// @param registry Registry that delivered the event; unused.
/// @param name Numeric global name.
/// @param interface Borrowed interface name valid during this callback.
/// @param version Advertised protocol version.
static void vgfx_wayland_registry_global(void *data,
                                         struct wl_registry *registry,
                                         uint32_t name,
                                         const char *interface,
                                         uint32_t version) {
    (void)registry;
    vgfx_wayland_connection_t *connection = (vgfx_wayland_connection_t *)data;
    if (!connection || !interface)
        return;
    if (strcmp(interface, "wl_compositor") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_COMPOSITOR;
        connection->compositor_name = name;
        connection->compositor_version = version;
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_SUBCOMPOSITOR;
        connection->subcompositor_name = name;
        connection->subcompositor_version = version;
    } else if (strcmp(interface, "wl_shm") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_SHM;
        connection->shm_name = name;
        connection->shm_version = version;
    } else if (strcmp(interface, "wl_seat") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_SEAT;
        connection->seat_name = name;
        connection->seat_version = version;
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_XDG_WM_BASE;
        connection->xdg_wm_base_name = name;
        connection->xdg_wm_base_version = version;
    } else if (strcmp(interface, "wl_data_device_manager") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_DATA_DEVICE_MANAGER;
        connection->data_device_manager_name = name;
        connection->data_device_manager_version = version;
    } else if (strcmp(interface, "zwp_text_input_manager_v3") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_TEXT_INPUT_MANAGER_V3;
        connection->text_input_manager_v3_name = name;
        connection->text_input_manager_v3_version = version;
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_VIEWPORTER;
        connection->viewporter_name = name;
        connection->viewporter_version = version;
    } else if (strcmp(interface, "wp_fractional_scale_manager_v1") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_FRACTIONAL_SCALE_V1;
        connection->fractional_scale_manager_v1_name = name;
        connection->fractional_scale_manager_v1_version = version;
    } else if (strcmp(interface, "zxdg_decoration_manager_v1") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_XDG_DECORATION_V1;
        connection->decoration_manager_v1_name = name;
        connection->decoration_manager_v1_version = version;
    } else if (strcmp(interface, "zwp_relative_pointer_manager_v1") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_RELATIVE_POINTER_V1;
        connection->relative_pointer_manager_v1_name = name;
        connection->relative_pointer_manager_v1_version = version;
    } else if (strcmp(interface, "zwp_pointer_constraints_v1") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_POINTER_CONSTRAINTS_V1;
        connection->pointer_constraints_v1_name = name;
        connection->pointer_constraints_v1_version = version;
    } else if (strcmp(interface, "xdg_activation_v1") == 0) {
        connection->globals |= VGFX_WAYLAND_GLOBAL_XDG_ACTIVATION_V1;
        connection->activation_v1_name = name;
        connection->activation_v1_version = version;
    } else if (strcmp(interface, "wl_output") == 0 && connection->output_count < 16) {
        uint32_t index = connection->output_count++;
        connection->output_names[index] = name;
        connection->output_versions[index] = version;
    }
    if (connection->global_observer)
        connection->global_observer(
            connection->global_observer_data, name, interface, version);
}

/// @brief Remove a departed output from the bounded registry inventory.
/// @details Uses swap removal and forwards every removal name to the borrowed
///          observer so subsystem-owned non-output globals can react as needed.
/// @param data Borrowed `vgfx_wayland_connection_t` listener context.
/// @param registry Registry that delivered the event; unused.
/// @param name Numeric global name that disappeared.
static void vgfx_wayland_registry_global_remove(void *data,
                                                struct wl_registry *registry,
                                                uint32_t name) {
    (void)registry;
    vgfx_wayland_connection_t *connection = (vgfx_wayland_connection_t *)data;
    if (!connection)
        return;
    for (uint32_t i = 0; i < connection->output_count; ++i) {
        if (connection->output_names[i] == name) {
            connection->output_count--;
            connection->output_names[i] = connection->output_names[connection->output_count];
            connection->output_versions[i] = connection->output_versions[connection->output_count];
            break;
        }
    }
    if (connection->global_remove_observer)
        connection->global_remove_observer(connection->global_observer_data, name);
}

static const vgfx_wl_registry_listener_t g_vgfx_wayland_registry_listener = {
    .global = vgfx_wayland_registry_global,
    .global_remove = vgfx_wayland_registry_global_remove,
};

/// @brief Answer an xdg-shell compositor ping.
/// @param data Borrowed connection listener context.
/// @param xdg_wm_base Shell manager that issued the ping.
/// @param serial Serial to return in the pong request.
static void vgfx_wayland_xdg_ping(void *data,
                                  struct xdg_wm_base *xdg_wm_base,
                                  uint32_t serial) {
    vgfx_wayland_connection_t *connection = (vgfx_wayland_connection_t *)data;
    if (connection)
        vgfx_xdg_wm_base_pong(&connection->api, xdg_wm_base, serial);
}

static const vgfx_xdg_wm_base_listener_t g_vgfx_wayland_xdg_listener = {
    .ping = vgfx_wayland_xdg_ping,
};

/// @copydoc vgfx_wayland_connection_close
void vgfx_wayland_connection_close(vgfx_wayland_connection_t *connection) {
    if (!connection)
        return;
    if (connection->xdg_wm_base && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->xdg_wm_base);
    if (connection->data_device_manager && connection->api.proxy_destroy)
        connection->api.proxy_destroy(connection->data_device_manager);
    if (connection->text_input_manager_v3 && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->text_input_manager_v3);
    if (connection->fractional_scale_manager_v1 && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->fractional_scale_manager_v1);
    if (connection->decoration_manager_v1 && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->decoration_manager_v1);
    if (connection->relative_pointer_manager_v1 && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->relative_pointer_manager_v1);
    if (connection->pointer_constraints_v1 && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->pointer_constraints_v1);
    if (connection->activation_v1 && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->activation_v1);
    if (connection->viewporter && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->viewporter);
    if (connection->seat && connection->api.proxy_destroy)
        connection->api.proxy_destroy(connection->seat);
    if (connection->shm && connection->api.proxy_destroy)
        connection->api.proxy_destroy(connection->shm);
    if (connection->subcompositor && connection->api.proxy_destroy)
        connection->api.proxy_destroy(connection->subcompositor);
    if (connection->compositor && connection->api.proxy_destroy)
        connection->api.proxy_destroy(connection->compositor);
    if (connection->registry && connection->api.proxy_destroy)
        connection->api.proxy_destroy((struct wl_proxy *)connection->registry);
    if (connection->display && connection->api.display_disconnect)
        connection->api.display_disconnect(connection->display);
    connection->registry = NULL;
    connection->display = NULL;
    vgfx_wayland_loader_close(&connection->api);
    memset(connection, 0, sizeof(*connection));
}

/// @copydoc vgfx_wayland_connection_open
int vgfx_wayland_connection_open(vgfx_wayland_connection_t *connection,
                                 const char *display_name,
                                 const char *library_name,
                                 char *error,
                                 uint32_t error_size) {
    const char *missing = NULL;
    if (error && error_size > 0)
        error[0] = '\0';
    if (!connection) {
        vgfx_wayland_connection_error(error, error_size, "connection destination is null");
        return 0;
    }
    memset(connection, 0, sizeof(*connection));
    if (!vgfx_wayland_loader_open(&connection->api, library_name, error, error_size))
        return 0;

    connection->display = connection->api.display_connect(display_name);
    if (!connection->display) {
        vgfx_wayland_connection_close(connection);
        vgfx_wayland_connection_error(error, error_size, "could not connect to compositor");
        return 0;
    }
    connection->registry = (struct wl_registry *)connection->api.proxy_marshal_flags(
        (struct wl_proxy *)connection->display,
        WL_DISPLAY_GET_REGISTRY,
        connection->api.registry_interface,
        connection->api.proxy_get_version((struct wl_proxy *)connection->display),
        0,
        NULL);
    if (!connection->registry ||
        connection->api.proxy_add_listener(
            (struct wl_proxy *)connection->registry,
            (void (**)(void))(void *)&g_vgfx_wayland_registry_listener,
            connection) != 0 ||
        connection->api.display_roundtrip(connection->display) < 0) {
        vgfx_wayland_connection_close(connection);
        vgfx_wayland_connection_error(error, error_size, "registry discovery failed");
        return 0;
    }

    if (!(connection->globals & VGFX_WAYLAND_GLOBAL_COMPOSITOR))
        missing = "wl_compositor";
    else if (!(connection->globals & VGFX_WAYLAND_GLOBAL_SHM))
        missing = "wl_shm";
    else if (!(connection->globals & VGFX_WAYLAND_GLOBAL_SEAT))
        missing = "wl_seat";
    else if (!(connection->globals & VGFX_WAYLAND_GLOBAL_XDG_WM_BASE))
        missing = "xdg_wm_base";
    if (missing) {
        char detail[128];
        (void)snprintf(detail, sizeof(detail), "compositor is missing %s", missing);
        vgfx_wayland_connection_close(connection);
        vgfx_wayland_connection_error(error, error_size, detail);
        return 0;
    }

    connection->compositor = vgfx_wayland_registry_bind(&connection->api,
                                                        connection->registry,
                                                        connection->compositor_name,
                                                        connection->api.compositor_interface,
                                                        connection->compositor_version,
                                                        6);
    if (connection->globals & VGFX_WAYLAND_GLOBAL_SUBCOMPOSITOR) {
        connection->subcompositor = vgfx_wayland_registry_bind(
            &connection->api,
            connection->registry,
            connection->subcompositor_name,
            connection->api.subcompositor_interface,
            connection->subcompositor_version,
            1);
    }
    connection->shm = vgfx_wayland_registry_bind(&connection->api,
                                                 connection->registry,
                                                 connection->shm_name,
                                                 connection->api.shm_interface,
                                                 connection->shm_version,
                                                 1);
    connection->seat = vgfx_wayland_registry_bind(&connection->api,
                                                  connection->registry,
                                                  connection->seat_name,
                                                  connection->api.seat_interface,
                                                  connection->seat_version,
                                                  8);
    connection->xdg_wm_base =
        (struct xdg_wm_base *)vgfx_wayland_registry_bind(&connection->api,
                                                         connection->registry,
                                                         connection->xdg_wm_base_name,
                                                         &vgfx_xdg_wm_base_interface,
                                                         connection->xdg_wm_base_version,
                                                         6);
    if (connection->globals & VGFX_WAYLAND_GLOBAL_DATA_DEVICE_MANAGER) {
        connection->data_device_manager =
            vgfx_wayland_registry_bind(&connection->api,
                                       connection->registry,
                                       connection->data_device_manager_name,
                                       connection->api.data_device_manager_interface,
                                       connection->data_device_manager_version,
                                       3);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_TEXT_INPUT_MANAGER_V3) {
        connection->text_input_manager_v3 =
            (struct zwp_text_input_manager_v3 *)vgfx_wayland_registry_bind(
                &connection->api,
                connection->registry,
                connection->text_input_manager_v3_name,
                &vgfx_zwp_text_input_manager_v3_interface,
                connection->text_input_manager_v3_version,
                1);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_VIEWPORTER) {
        connection->viewporter = (struct wp_viewporter *)vgfx_wayland_registry_bind(
            &connection->api,
            connection->registry,
            connection->viewporter_name,
            &vgfx_wp_viewporter_interface,
            connection->viewporter_version,
            1);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_FRACTIONAL_SCALE_V1) {
        connection->fractional_scale_manager_v1 =
            (struct wp_fractional_scale_manager_v1 *)vgfx_wayland_registry_bind(
                &connection->api,
                connection->registry,
                connection->fractional_scale_manager_v1_name,
                &vgfx_wp_fractional_scale_manager_v1_interface,
                connection->fractional_scale_manager_v1_version,
                1);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_XDG_DECORATION_V1) {
        connection->decoration_manager_v1 =
            (struct zxdg_decoration_manager_v1 *)vgfx_wayland_registry_bind(
                &connection->api,
                connection->registry,
                connection->decoration_manager_v1_name,
                &vgfx_zxdg_decoration_manager_v1_interface,
                connection->decoration_manager_v1_version,
                1);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_RELATIVE_POINTER_V1) {
        connection->relative_pointer_manager_v1 =
            (struct zwp_relative_pointer_manager_v1 *)vgfx_wayland_registry_bind(
                &connection->api,
                connection->registry,
                connection->relative_pointer_manager_v1_name,
                &vgfx_zwp_relative_pointer_manager_v1_interface,
                connection->relative_pointer_manager_v1_version,
                1);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_POINTER_CONSTRAINTS_V1) {
        connection->pointer_constraints_v1 =
            (struct zwp_pointer_constraints_v1 *)vgfx_wayland_registry_bind(
                &connection->api,
                connection->registry,
                connection->pointer_constraints_v1_name,
                &vgfx_zwp_pointer_constraints_v1_interface,
                connection->pointer_constraints_v1_version,
                1);
    }
    if (connection->globals & VGFX_WAYLAND_GLOBAL_XDG_ACTIVATION_V1) {
        connection->activation_v1 = (struct xdg_activation_v1 *)vgfx_wayland_registry_bind(
            &connection->api,
            connection->registry,
            connection->activation_v1_name,
            &vgfx_xdg_activation_v1_interface,
            connection->activation_v1_version,
            1);
    }
    if (!connection->compositor || !connection->shm || !connection->seat ||
        !connection->xdg_wm_base ||
        vgfx_xdg_wm_base_add_listener(&connection->api,
                                      connection->xdg_wm_base,
                                      &g_vgfx_wayland_xdg_listener,
                                      connection) != 0 ||
        connection->api.display_roundtrip(connection->display) < 0) {
        vgfx_wayland_connection_close(connection);
        vgfx_wayland_connection_error(error, error_size, "required global binding failed");
        return 0;
    }
    return 1;
}
