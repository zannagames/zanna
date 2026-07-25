//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_connection.h
// Purpose: Own a Wayland display connection and discover compositor globals.
// Key invariants:
//   - Successful connections have completed one registry round trip.
//   - Disconnect destroys proxies before unloading the client dispatch table.
// Ownership/Lifetime:
//   - One connection owns its display, registry proxy, and dynamic loader.
//   - Registry interface names are copied into capability bits, never retained.
// Links: src/lib/graphics/src/vgfx_wayland_loader.h,
//        docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Dynamically loaded Wayland connection, registry, and global inventory.
/// @details Defines the connection object that owns the client dispatch table,
///          display, registry, and every bound singleton protocol global while
///          recording advertised names/versions for optional subsystem setup.

#pragma once

#include "vgfx_wayland_loader.h"
#include "vgfx_wayland_protocol.h"

#include <stdint.h>

struct wl_registry;

/// @brief Bit flags describing compositor globals observed in the initial registry.
enum {
    VGFX_WAYLAND_GLOBAL_COMPOSITOR = 1u << 0,
    VGFX_WAYLAND_GLOBAL_SHM = 1u << 1,
    VGFX_WAYLAND_GLOBAL_SEAT = 1u << 2,
    VGFX_WAYLAND_GLOBAL_XDG_WM_BASE = 1u << 3,
    VGFX_WAYLAND_GLOBAL_DATA_DEVICE_MANAGER = 1u << 4,
    VGFX_WAYLAND_GLOBAL_TEXT_INPUT_MANAGER_V3 = 1u << 5,
    VGFX_WAYLAND_GLOBAL_VIEWPORTER = 1u << 6,
    VGFX_WAYLAND_GLOBAL_FRACTIONAL_SCALE_V1 = 1u << 7,
    VGFX_WAYLAND_GLOBAL_XDG_DECORATION_V1 = 1u << 8,
    VGFX_WAYLAND_GLOBAL_RELATIVE_POINTER_V1 = 1u << 9,
    VGFX_WAYLAND_GLOBAL_POINTER_CONSTRAINTS_V1 = 1u << 10,
    VGFX_WAYLAND_GLOBAL_XDG_ACTIVATION_V1 = 1u << 11,
    VGFX_WAYLAND_GLOBAL_SUBCOMPOSITOR = 1u << 12,
    VGFX_WAYLAND_REQUIRED_GLOBALS = VGFX_WAYLAND_GLOBAL_COMPOSITOR | VGFX_WAYLAND_GLOBAL_SHM |
                                    VGFX_WAYLAND_GLOBAL_SEAT | VGFX_WAYLAND_GLOBAL_XDG_WM_BASE,
};

/// @brief Owned Wayland connection and discovered protocol-global state.
/// @details Required and optional singleton proxies are owned until
///          `vgfx_wayland_connection_close()`.  Output name/version arrays hold
///          at most sixteen live registry entries.  Observer callbacks and their
///          context are borrowed and invoked synchronously during dispatch.
typedef struct vgfx_wayland_connection {
    /// @brief Dynamically loaded Wayland client API owned by this connection.
    vgfx_wayland_client_api_t api;
    /// @brief Owned display connection and registry proxy.
    struct wl_display *display;
    struct wl_registry *registry;
    /// @brief Owned bound core protocol singleton proxies.
    struct wl_proxy *compositor;
    struct wl_proxy *subcompositor;
    struct wl_proxy *shm;
    struct wl_proxy *seat;
    struct wl_proxy *data_device_manager;
    /// @brief Owned optional extension-manager singleton proxies.
    struct zwp_text_input_manager_v3 *text_input_manager_v3;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager_v1;
    struct zxdg_decoration_manager_v1 *decoration_manager_v1;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager_v1;
    struct zwp_pointer_constraints_v1 *pointer_constraints_v1;
    struct xdg_activation_v1 *activation_v1;
    /// @brief Owned required xdg-shell manager proxy.
    struct xdg_wm_base *xdg_wm_base;
    /// @brief Bitwise `VGFX_WAYLAND_GLOBAL_*` inventory.
    uint32_t globals;
    /// @brief Registry names and advertised versions for singleton globals.
    uint32_t compositor_name;
    uint32_t compositor_version;
    uint32_t subcompositor_name;
    uint32_t subcompositor_version;
    uint32_t shm_name;
    uint32_t shm_version;
    uint32_t seat_name;
    uint32_t seat_version;
    uint32_t xdg_wm_base_name;
    uint32_t xdg_wm_base_version;
    uint32_t data_device_manager_name;
    uint32_t data_device_manager_version;
    uint32_t text_input_manager_v3_name;
    uint32_t text_input_manager_v3_version;
    uint32_t viewporter_name;
    uint32_t viewporter_version;
    uint32_t fractional_scale_manager_v1_name;
    uint32_t fractional_scale_manager_v1_version;
    uint32_t decoration_manager_v1_name;
    uint32_t decoration_manager_v1_version;
    uint32_t relative_pointer_manager_v1_name;
    uint32_t relative_pointer_manager_v1_version;
    uint32_t pointer_constraints_v1_name;
    uint32_t pointer_constraints_v1_version;
    uint32_t activation_v1_name;
    uint32_t activation_v1_version;
    /// @brief Bounded registry names and versions for advertised outputs.
    uint32_t output_names[16];
    uint32_t output_versions[16];
    uint32_t output_count;
    /// @brief Borrowed callbacks notified as globals appear or disappear.
    void (*global_observer)(void *data,
                            uint32_t name,
                            const char *interface,
                            uint32_t version);
    void (*global_remove_observer)(void *data, uint32_t name);
    void *global_observer_data;
} vgfx_wayland_connection_t;

/// @brief Load Wayland, connect, and inventory the initial registry globals.
/// @param connection Destination connection, zeroed on failure.
/// @param display_name Optional Wayland display name; NULL uses WAYLAND_DISPLAY/default policy.
/// @param library_name Optional test override for the client shared-object name.
/// @param error Destination for a stable failure diagnostic.
/// @param error_size Capacity of error including its terminator.
/// @return 1 when all required globals are advertised, otherwise 0.
int vgfx_wayland_connection_open(vgfx_wayland_connection_t *connection,
                                 const char *display_name,
                                 const char *library_name,
                                 char *error,
                                 uint32_t error_size);

/// @brief Disconnect and zero a connection, accepting NULL and partial state.
/// @details Destroys bound proxies before the registry and display, unloads the
///          dynamic dispatch table, and clears every field.
/// @param connection Connection to close; NULL is ignored.
void vgfx_wayland_connection_close(vgfx_wayland_connection_t *connection);
