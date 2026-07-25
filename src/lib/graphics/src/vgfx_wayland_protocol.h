//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_protocol.h
// Purpose: Repository-owned stable xdg-shell client protocol metadata and requests.
// Key invariants:
//   - Metadata matches stable xdg-shell through interface version 6.
//   - Requests use only the dynamically resolved Wayland client ABI.
// Ownership/Lifetime:
//   - Interface descriptors have process lifetime and own no dynamic memory.
//   - Returned proxies are owned by the caller and tied to its display connection.
// Links: stable/xdg-shell/xdg-shell.xml,
//        docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "vgfx_wayland_loader.h"

/// @file
/// @brief Declares repository-owned Wayland extension metadata and xdg-shell helpers.
/// @details Zanna supplies these protocol descriptors directly instead of depending on
/// `wayland-scanner` output. The declarations cover stable xdg-shell plus the optional
/// protocols used for text input, scaling, decorations, relative pointers, pointer locking,
/// and activation.

/// @brief Opaque core Wayland output object.
struct wl_output;

/// @brief Opaque core Wayland registry object.
struct wl_registry;

/// @brief Opaque core Wayland input-seat object.
struct wl_seat;

/// @brief Opaque core Wayland drawing surface.
struct wl_surface;

/// @brief Opaque xdg-shell popup role.
struct xdg_popup;

/// @brief Opaque text-input-v3 manager.
struct zwp_text_input_manager_v3;

/// @brief Opaque text-input-v3 context.
struct zwp_text_input_v3;

/// @brief Opaque viewporter protocol manager.
struct wp_viewporter;

/// @brief Opaque viewport assigned to one surface.
struct wp_viewport;

/// @brief Opaque fractional-scale protocol manager.
struct wp_fractional_scale_manager_v1;

/// @brief Opaque fractional-scale object assigned to one surface.
struct wp_fractional_scale_v1;

/// @brief Opaque xdg-decoration protocol manager.
struct zxdg_decoration_manager_v1;

/// @brief Opaque decoration object assigned to an xdg toplevel.
struct zxdg_toplevel_decoration_v1;

/// @brief Opaque relative-pointer protocol manager.
struct zwp_relative_pointer_manager_v1;

/// @brief Opaque relative-motion event source.
struct zwp_relative_pointer_v1;

/// @brief Opaque pointer-constraints protocol manager.
struct zwp_pointer_constraints_v1;

/// @brief Opaque locked-pointer constraint.
struct zwp_locked_pointer_v1;

/// @brief Opaque xdg-activation protocol manager.
struct xdg_activation_v1;

/// @brief Opaque xdg-activation token builder.
struct xdg_activation_token_v1;

/// @brief Opaque xdg-shell popup geometry description.
struct xdg_positioner;

/// @brief Opaque xdg-shell role wrapper for a core surface.
struct xdg_surface;

/// @brief Opaque xdg-shell toplevel window role.
struct xdg_toplevel;

/// @brief Opaque xdg-shell compositor global.
struct xdg_wm_base;

/// @brief Stable xdg-shell compositor interface, version 6.
extern const struct wl_interface vgfx_xdg_wm_base_interface;

/// @brief Stable xdg-shell positioner interface, version 6.
extern const struct wl_interface vgfx_xdg_positioner_interface;

/// @brief Stable xdg-shell surface interface, version 6.
extern const struct wl_interface vgfx_xdg_surface_interface;

/// @brief Stable xdg-shell toplevel interface, version 6.
extern const struct wl_interface vgfx_xdg_toplevel_interface;

/// @brief Stable xdg-shell popup interface, version 6.
extern const struct wl_interface vgfx_xdg_popup_interface;

/// @brief Unstable text-input-v3 manager interface, version 1.
extern const struct wl_interface vgfx_zwp_text_input_manager_v3_interface;

/// @brief Unstable text-input-v3 context interface, version 1.
extern const struct wl_interface vgfx_zwp_text_input_v3_interface;

/// @brief Stable viewporter manager interface, version 1.
extern const struct wl_interface vgfx_wp_viewporter_interface;

/// @brief Stable per-surface viewport interface, version 1.
extern const struct wl_interface vgfx_wp_viewport_interface;

/// @brief Stable fractional-scale manager interface, version 1.
extern const struct wl_interface vgfx_wp_fractional_scale_manager_v1_interface;

/// @brief Stable per-surface fractional-scale interface, version 1.
extern const struct wl_interface vgfx_wp_fractional_scale_v1_interface;

/// @brief Unstable xdg-decoration manager interface, version 1.
extern const struct wl_interface vgfx_zxdg_decoration_manager_v1_interface;

/// @brief Unstable per-toplevel xdg-decoration interface, version 1.
extern const struct wl_interface vgfx_zxdg_toplevel_decoration_v1_interface;

/// @brief Unstable relative-pointer manager interface, version 1.
extern const struct wl_interface vgfx_zwp_relative_pointer_manager_v1_interface;

/// @brief Unstable relative-pointer event interface, version 1.
extern const struct wl_interface vgfx_zwp_relative_pointer_v1_interface;

/// @brief Unstable pointer-constraints manager interface, version 1.
extern const struct wl_interface vgfx_zwp_pointer_constraints_v1_interface;

/// @brief Unstable locked-pointer constraint interface, version 1.
extern const struct wl_interface vgfx_zwp_locked_pointer_v1_interface;

/// @brief Stable xdg-activation manager interface, version 1.
extern const struct wl_interface vgfx_xdg_activation_v1_interface;

/// @brief Stable xdg-activation token interface, version 1.
extern const struct wl_interface vgfx_xdg_activation_token_v1_interface;

/// @brief Listener callbacks emitted by an `xdg_wm_base` proxy.
typedef struct vgfx_xdg_wm_base_listener {
    /// @brief Request a liveness reply from the client.
    /// @param data User data registered with @ref vgfx_xdg_wm_base_add_listener.
    /// @param xdg_wm_base Compositor global that issued the ping.
    /// @param serial Serial that must be returned with @ref vgfx_xdg_wm_base_pong.
    void (*ping)(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial);
} vgfx_xdg_wm_base_listener_t;

/// @brief Wire opcodes and marshal flags used by the backend's core and xdg-shell requests.
enum {
    /// @brief Tell `wl_proxy_marshal_flags` to destroy the proxy after marshalling.
    VGFX_WL_MARSHAL_FLAG_DESTROY = 1,

    /// @brief Opcode for `wl_registry.bind`.
    VGFX_WL_REGISTRY_BIND = 0,

    /// @brief Opcode for `wl_compositor.create_surface`.
    VGFX_WL_COMPOSITOR_CREATE_SURFACE = 0,

    /// @brief Opcode for `wl_surface.destroy`.
    VGFX_WL_SURFACE_DESTROY = 0,

    /// @brief Opcode for `wl_surface.attach`.
    VGFX_WL_SURFACE_ATTACH = 1,

    /// @brief Opcode for legacy `wl_surface.damage`.
    VGFX_WL_SURFACE_DAMAGE = 2,

    /// @brief Opcode for `wl_surface.frame`.
    VGFX_WL_SURFACE_FRAME = 3,

    /// @brief Opcode for `wl_surface.commit`.
    VGFX_WL_SURFACE_COMMIT = 6,

    /// @brief Opcode for `xdg_wm_base.destroy`.
    VGFX_XDG_WM_BASE_DESTROY = 0,

    /// @brief Opcode for `xdg_wm_base.get_xdg_surface`.
    VGFX_XDG_WM_BASE_GET_XDG_SURFACE = 2,

    /// @brief Opcode for `xdg_wm_base.pong`.
    VGFX_XDG_WM_BASE_PONG = 3,

    /// @brief Opcode for `xdg_surface.destroy`.
    VGFX_XDG_SURFACE_DESTROY = 0,

    /// @brief Opcode for `xdg_surface.get_toplevel`.
    VGFX_XDG_SURFACE_GET_TOPLEVEL = 1,

    /// @brief Opcode for `xdg_surface.ack_configure`.
    VGFX_XDG_SURFACE_ACK_CONFIGURE = 4,

    /// @brief Opcode for `xdg_toplevel.destroy`.
    VGFX_XDG_TOPLEVEL_DESTROY = 0,

    /// @brief Opcode for `xdg_toplevel.set_title`.
    VGFX_XDG_TOPLEVEL_SET_TITLE = 2,

    /// @brief Opcode for `xdg_toplevel.set_app_id`.
    VGFX_XDG_TOPLEVEL_SET_APP_ID = 3,

    /// @brief Opcode for `xdg_toplevel.set_max_size`.
    VGFX_XDG_TOPLEVEL_SET_MAX_SIZE = 7,

    /// @brief Opcode for `xdg_toplevel.set_min_size`.
    VGFX_XDG_TOPLEVEL_SET_MIN_SIZE = 8,

    /// @brief Opcode for `xdg_toplevel.set_maximized`.
    VGFX_XDG_TOPLEVEL_SET_MAXIMIZED = 9,

    /// @brief Opcode for `xdg_toplevel.unset_maximized`.
    VGFX_XDG_TOPLEVEL_UNSET_MAXIMIZED = 10,

    /// @brief Opcode for `xdg_toplevel.set_fullscreen`.
    VGFX_XDG_TOPLEVEL_SET_FULLSCREEN = 11,

    /// @brief Opcode for `xdg_toplevel.unset_fullscreen`.
    VGFX_XDG_TOPLEVEL_UNSET_FULLSCREEN = 12,

    /// @brief Opcode for `xdg_toplevel.set_minimized`.
    VGFX_XDG_TOPLEVEL_SET_MINIMIZED = 13,
};

/// @brief Bind a registry global, capping the requested version at the caller's tested version.
/// @details The request uses the lower of @p advertised_version and @p tested_version so the
/// returned proxy never exposes protocol behavior that either the compositor or this backend
/// does not support.
/// @param api Loaded Wayland client dispatch table.
/// @param registry Registry proxy that advertised the global.
/// @param name Compositor-assigned numeric name of the global.
/// @param interface Protocol interface expected for the global.
/// @param advertised_version Highest version advertised by the compositor.
/// @param tested_version Highest version supported by the caller.
/// @return Newly allocated proxy owned by the caller, or NULL for invalid input or marshal
/// failure.
struct wl_proxy *vgfx_wayland_registry_bind(const vgfx_wayland_client_api_t *api,
                                             struct wl_registry *registry,
                                             uint32_t name,
                                             const struct wl_interface *interface,
                                             uint32_t advertised_version,
                                             uint32_t tested_version);

/// @brief Add the one-event xdg_wm_base listener.
/// @param api Loaded Wayland client dispatch table.
/// @param wm_base xdg-shell compositor proxy receiving the listener.
/// @param listener Callback table that must remain valid for the proxy lifetime.
/// @param data Opaque callback context returned to @ref vgfx_xdg_wm_base_listener_t::ping.
/// @return The Wayland client result, or -1 when a required argument or entry point is absent.
int vgfx_xdg_wm_base_add_listener(const vgfx_wayland_client_api_t *api,
                                  struct xdg_wm_base *wm_base,
                                  const vgfx_xdg_wm_base_listener_t *listener,
                                  void *data);

/// @brief Reply to an xdg_wm_base ping.
/// @details Invalid arguments are ignored because a missing pong target cannot be recovered.
/// @param api Loaded Wayland client dispatch table.
/// @param wm_base xdg-shell compositor proxy that issued the ping.
/// @param serial Serial received by @ref vgfx_xdg_wm_base_listener_t::ping.
void vgfx_xdg_wm_base_pong(const vgfx_wayland_client_api_t *api,
                           struct xdg_wm_base *wm_base,
                           uint32_t serial);
