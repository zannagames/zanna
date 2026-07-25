//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_shell.c
// Purpose: Create and manage a stable xdg-shell toplevel role.
// Key invariants:
//   - The empty initial commit precedes waiting for the first configure event.
//   - Every configure serial is acknowledged before future content is committed.
// Ownership/Lifetime: See vgfx_wayland_shell.h.
// Links: stable/xdg-shell/xdg-shell.xml
//
//===----------------------------------------------------------------------===//

#include "vgfx_wayland_shell.h"

#include <stdio.h>
#include <string.h>

/// @file
/// @brief Implements xdg-shell toplevel creation, configuration, and teardown.

/// @brief ABI callback table for `xdg_surface` events.
typedef struct vgfx_xdg_surface_listener {
    /// @brief Receive and acknowledge a compositor configure serial.
    void (*configure)(void *data, struct xdg_surface *xdg_surface, uint32_t serial);
} vgfx_xdg_surface_listener_t;

/// @brief ABI callback table for stable `xdg_toplevel` events through version 6.
typedef struct vgfx_xdg_toplevel_listener {
    /// @brief Receive a proposed logical size and packed window-state array.
    void (*configure)(void *data,
                      struct xdg_toplevel *toplevel,
                      int32_t width,
                      int32_t height,
                      struct wl_array *states);

    /// @brief Receive a compositor request to close the window.
    void (*close)(void *data, struct xdg_toplevel *toplevel);

    /// @brief Receive compositor-recommended toplevel size bounds.
    void (*configure_bounds)(void *data,
                             struct xdg_toplevel *toplevel,
                             int32_t width,
                             int32_t height);

    /// @brief Receive the compositor's supported window-management operations.
    void (*wm_capabilities)(void *data,
                            struct xdg_toplevel *toplevel,
                            struct wl_array *capabilities);
} vgfx_xdg_toplevel_listener_t;

/// @brief ABI callback table for core `wl_surface` events through version 6.
typedef struct vgfx_wl_surface_listener {
    /// @brief Report that the surface began overlapping an output.
    void (*enter)(void *data, struct wl_surface *surface, struct wl_proxy *output);

    /// @brief Report that the surface stopped overlapping an output.
    void (*leave)(void *data, struct wl_surface *surface, struct wl_proxy *output);

    /// @brief Report the compositor's preferred integer buffer scale.
    void (*preferred_buffer_scale)(void *data, struct wl_surface *surface, int32_t factor);

    /// @brief Report the compositor's preferred buffer transform.
    void (*preferred_buffer_transform)(void *data, struct wl_surface *surface, uint32_t transform);
} vgfx_wl_surface_listener_t;

/// @brief ABI callback table for xdg-decoration mode negotiation.
typedef struct vgfx_zxdg_toplevel_decoration_listener {
    /// @brief Receive the compositor-selected client-side or server-side mode.
    void (*configure)(void *data,
                      struct zxdg_toplevel_decoration_v1 *decoration,
                      uint32_t mode);
} vgfx_zxdg_toplevel_decoration_listener_t;

/// @brief xdg-shell state values and xdg-decoration request constants.
enum {
    /// @brief `xdg_toplevel` state value indicating a maximized window.
    VGFX_XDG_TOPLEVEL_STATE_MAXIMIZED = 1,

    /// @brief `xdg_toplevel` state value indicating a fullscreen window.
    VGFX_XDG_TOPLEVEL_STATE_FULLSCREEN = 2,

    /// @brief `xdg_toplevel` state value indicating an interactive resize.
    VGFX_XDG_TOPLEVEL_STATE_RESIZING = 3,

    /// @brief `xdg_toplevel` state value indicating keyboard activation.
    VGFX_XDG_TOPLEVEL_STATE_ACTIVATED = 4,

    /// @brief Opcode for `zxdg_decoration_manager_v1.get_toplevel_decoration`.
    VGFX_ZXDG_DECORATION_MANAGER_GET_TOPLEVEL_DECORATION = 1,

    /// @brief Opcode for `zxdg_toplevel_decoration_v1.destroy`.
    VGFX_ZXDG_TOPLEVEL_DECORATION_DESTROY = 0,

    /// @brief Opcode for `zxdg_toplevel_decoration_v1.set_mode`.
    VGFX_ZXDG_TOPLEVEL_DECORATION_SET_MODE = 1,

    /// @brief Decoration protocol value requesting compositor-rendered chrome.
    VGFX_ZXDG_TOPLEVEL_DECORATION_MODE_SERVER_SIDE = 2,
};

/// @brief Store the decoration mode selected by the compositor.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param decoration Decoration proxy that emitted the event.
/// @param mode Protocol decoration-mode value.
static void vgfx_wayland_decoration_configure(
    void *data, struct zxdg_toplevel_decoration_v1 *decoration, uint32_t mode) {
    (void)decoration;
    vgfx_wayland_shell_t *shell = (vgfx_wayland_shell_t *)data;
    if (shell)
        shell->decoration_mode = mode;
}

static const vgfx_zxdg_toplevel_decoration_listener_t g_vgfx_decoration_listener = {
    .configure = vgfx_wayland_decoration_configure,
};

/// @brief Write a stable window-creation diagnostic into a caller buffer.
/// @param error Destination buffer, or NULL.
/// @param size Capacity of @p error in bytes, including its terminator.
/// @param detail Specific non-null failure description.
static void vgfx_wayland_shell_error(char *error, uint32_t size, const char *detail) {
    if (error && size > 0)
        (void)snprintf(error, (size_t)size, "Wayland window creation failed: %s", detail);
}

/// @brief Query the negotiated protocol version of a shell-owned proxy.
/// @param shell Shell whose connection provides the dispatch entry point.
/// @param proxy Non-null protocol proxy.
/// @return Version negotiated for @p proxy.
static uint32_t vgfx_wayland_proxy_version(vgfx_wayland_shell_t *shell, void *proxy) {
    return shell->connection->api.proxy_get_version((struct wl_proxy *)proxy);
}

/// @brief Acknowledge an xdg-surface configure and mark the initial handshake complete.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param xdg_surface Surface role that emitted the configure event.
/// @param serial Configure serial that must be acknowledged before committing content.
static void vgfx_wayland_xdg_surface_configure(void *data,
                                               struct xdg_surface *xdg_surface,
                                               uint32_t serial) {
    vgfx_wayland_shell_t *shell = (vgfx_wayland_shell_t *)data;
    if (!shell || !shell->connection)
        return;
    (void)shell->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)xdg_surface,
        VGFX_XDG_SURFACE_ACK_CONFIGURE,
        NULL,
        vgfx_wayland_proxy_version(shell, xdg_surface),
        0,
        serial);
    shell->configured = 1;
}

/// @brief Apply the compositor's latest toplevel size and state proposal.
/// @details Zero dimensions mean the client may retain its current size. State flags are
/// cleared before parsing the packed array so the structure represents only this configure.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param toplevel Toplevel proxy that emitted the event.
/// @param width Proposed logical width, or zero when unconstrained.
/// @param height Proposed logical height, or zero when unconstrained.
/// @param states Packed array of 32-bit xdg-toplevel state values.
static void vgfx_wayland_xdg_toplevel_configure(void *data,
                                                struct xdg_toplevel *toplevel,
                                                int32_t width,
                                                int32_t height,
                                                struct wl_array *states) {
    (void)toplevel;
    vgfx_wayland_shell_t *shell = (vgfx_wayland_shell_t *)data;
    if (!shell)
        return;
    if (width > 0)
        shell->width = width;
    if (height > 0)
        shell->height = height;
    shell->maximized = 0;
    shell->fullscreen = 0;
    shell->resizing = 0;
    shell->activated = 0;
    if (!states || !states->data)
        return;
    const uint32_t *state = (const uint32_t *)states->data;
    size_t count = states->size / sizeof(*state);
    for (size_t i = 0; i < count; ++i) {
        if (state[i] == VGFX_XDG_TOPLEVEL_STATE_MAXIMIZED)
            shell->maximized = 1;
        else if (state[i] == VGFX_XDG_TOPLEVEL_STATE_FULLSCREEN)
            shell->fullscreen = 1;
        else if (state[i] == VGFX_XDG_TOPLEVEL_STATE_RESIZING)
            shell->resizing = 1;
        else if (state[i] == VGFX_XDG_TOPLEVEL_STATE_ACTIVATED)
            shell->activated = 1;
    }
}

/// @brief Record the compositor's request to close the toplevel.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param toplevel Toplevel proxy that emitted the event.
static void vgfx_wayland_xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    vgfx_wayland_shell_t *shell = (vgfx_wayland_shell_t *)data;
    if (shell)
        shell->close_requested = 1;
}

/// @brief Consume optional compositor size bounds not currently used by window policy.
/// @param data Registered shell callback context.
/// @param toplevel Toplevel proxy that emitted the event.
/// @param width Recommended maximum content width.
/// @param height Recommended maximum content height.
static void vgfx_wayland_xdg_toplevel_configure_bounds(void *data,
                                                       struct xdg_toplevel *toplevel,
                                                       int32_t width,
                                                       int32_t height) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
}

/// @brief Consume optional compositor window-management capabilities.
/// @param data Registered shell callback context.
/// @param toplevel Toplevel proxy that emitted the event.
/// @param capabilities Packed array of supported xdg-toplevel operations.
static void vgfx_wayland_xdg_toplevel_wm_capabilities(void *data,
                                                      struct xdg_toplevel *toplevel,
                                                      struct wl_array *capabilities) {
    (void)data;
    (void)toplevel;
    (void)capabilities;
}

static const vgfx_xdg_surface_listener_t g_vgfx_xdg_surface_listener = {
    .configure = vgfx_wayland_xdg_surface_configure,
};

static const vgfx_xdg_toplevel_listener_t g_vgfx_xdg_toplevel_listener = {
    .configure = vgfx_wayland_xdg_toplevel_configure,
    .close = vgfx_wayland_xdg_toplevel_close,
    .configure_bounds = vgfx_wayland_xdg_toplevel_configure_bounds,
    .wm_capabilities = vgfx_wayland_xdg_toplevel_wm_capabilities,
};

/// @brief Track surface entry into an output and notify the optional observer.
/// @details Duplicate entries are ignored. The fixed inventory retains at most 16 output
/// proxies, but the observer is still notified for a new output when the inventory is full.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param surface Core surface that emitted the event.
/// @param output Output proxy newly containing the surface.
static void vgfx_wayland_surface_enter(void *data,
                                       struct wl_surface *surface,
                                       struct wl_proxy *output) {
    (void)surface;
    vgfx_wayland_shell_t *shell = data;
    if (!shell || !output)
        return;
    for (uint32_t i = 0; i < shell->entered_output_count; ++i)
        if (shell->entered_outputs[i] == output)
            return;
    if (shell->entered_output_count < 16)
        shell->entered_outputs[shell->entered_output_count++] = output;
    if (shell->output_observer)
        shell->output_observer(shell->output_observer_data, output, 1);
}

/// @brief Remove an output from surface membership and notify the optional observer.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param surface Core surface that emitted the event.
/// @param output Output proxy no longer containing the surface.
static void vgfx_wayland_surface_leave(void *data,
                                       struct wl_surface *surface,
                                       struct wl_proxy *output) {
    (void)surface;
    vgfx_wayland_shell_t *shell = data;
    if (!shell || !output)
        return;
    for (uint32_t i = 0; i < shell->entered_output_count; ++i) {
        if (shell->entered_outputs[i] == output) {
            shell->entered_output_count--;
            shell->entered_outputs[i] = shell->entered_outputs[shell->entered_output_count];
            break;
        }
    }
    if (shell->output_observer)
        shell->output_observer(shell->output_observer_data, output, 0);
}

/// @brief Forward a core preferred buffer scale to the registered scaling observer.
/// @param data Registered @ref vgfx_wayland_shell_t callback context.
/// @param surface Core surface that emitted the event.
/// @param factor Compositor-preferred positive integer scale.
static void vgfx_wayland_surface_preferred_scale(void *data,
                                                 struct wl_surface *surface,
                                                 int32_t factor) {
    (void)surface;
    vgfx_wayland_shell_t *shell = data;
    if (shell && shell->preferred_scale_observer)
        shell->preferred_scale_observer(shell->preferred_scale_observer_data, factor);
}

/// @brief Consume a preferred transform event not currently used by the renderer.
/// @param data Registered shell callback context.
/// @param surface Core surface that emitted the event.
/// @param transform Preferred core-protocol buffer transform.
static void vgfx_wayland_surface_preferred_transform(void *data,
                                                     struct wl_surface *surface,
                                                     uint32_t transform) {
    (void)data;
    (void)surface;
    (void)transform;
}

static const vgfx_wl_surface_listener_t g_vgfx_wayland_surface_listener = {
    vgfx_wayland_surface_enter,
    vgfx_wayland_surface_leave,
    vgfx_wayland_surface_preferred_scale,
    vgfx_wayland_surface_preferred_transform,
};

/// @brief Marshal the appropriate destructor for one shell-owned protocol object.
/// @param shell Shell whose connection supplies the marshal dispatch.
/// @param proxy Owned proxy to destroy, or NULL.
/// @param opcode Destructor request opcode appropriate for @p proxy.
static void vgfx_wayland_destroy_request(vgfx_wayland_shell_t *shell,
                                         void *proxy,
                                         uint32_t opcode) {
    if (!shell || !shell->connection || !proxy)
        return;
    (void)shell->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)proxy,
        opcode,
        NULL,
        vgfx_wayland_proxy_version(shell, proxy),
        VGFX_WL_MARSHAL_FLAG_DESTROY);
}

/// @copydoc vgfx_wayland_shell_close
void vgfx_wayland_shell_close(vgfx_wayland_shell_t *shell) {
    if (!shell)
        return;
    if (shell->decoration)
        vgfx_wayland_destroy_request(
            shell, shell->decoration, VGFX_ZXDG_TOPLEVEL_DECORATION_DESTROY);
    if (shell->toplevel)
        vgfx_wayland_destroy_request(shell, shell->toplevel, VGFX_XDG_TOPLEVEL_DESTROY);
    if (shell->xdg_surface)
        vgfx_wayland_destroy_request(shell, shell->xdg_surface, VGFX_XDG_SURFACE_DESTROY);
    if (shell->surface)
        vgfx_wayland_destroy_request(shell, shell->surface, VGFX_WL_SURFACE_DESTROY);
    memset(shell, 0, sizeof(*shell));
}

/// @copydoc vgfx_wayland_shell_open
int vgfx_wayland_shell_open(vgfx_wayland_shell_t *shell,
                            vgfx_wayland_connection_t *connection,
                            const char *title,
                            const char *app_id,
                            char *error,
                            uint32_t error_size) {
    if (error && error_size > 0)
        error[0] = '\0';
    if (!shell || !connection || !connection->compositor || !connection->xdg_wm_base) {
        vgfx_wayland_shell_error(error, error_size, "invalid connection");
        return 0;
    }
    memset(shell, 0, sizeof(*shell));
    shell->connection = connection;
    shell->surface = (struct wl_surface *)connection->api.proxy_marshal_flags(
        connection->compositor,
        VGFX_WL_COMPOSITOR_CREATE_SURFACE,
        connection->api.surface_interface,
        connection->api.proxy_get_version(connection->compositor),
        0,
        NULL);
    if (shell->surface &&
        connection->api.proxy_add_listener((struct wl_proxy *)shell->surface,
                                           (void (**)(void))(void *)&g_vgfx_wayland_surface_listener,
                                           shell) != 0) {
        vgfx_wayland_shell_close(shell);
        vgfx_wayland_shell_error(error, error_size, "could not listen to surface output changes");
        return 0;
    }
    if (shell->surface) {
        shell->xdg_surface =
            (struct xdg_surface *)connection->api.proxy_marshal_flags(
                (struct wl_proxy *)connection->xdg_wm_base,
                VGFX_XDG_WM_BASE_GET_XDG_SURFACE,
                &vgfx_xdg_surface_interface,
                connection->api.proxy_get_version((struct wl_proxy *)connection->xdg_wm_base),
                0,
                NULL,
                shell->surface);
    }
    if (shell->xdg_surface) {
        shell->toplevel = (struct xdg_toplevel *)connection->api.proxy_marshal_flags(
            (struct wl_proxy *)shell->xdg_surface,
            VGFX_XDG_SURFACE_GET_TOPLEVEL,
            &vgfx_xdg_toplevel_interface,
            vgfx_wayland_proxy_version(shell, shell->xdg_surface),
            0,
            NULL);
    }
    if (!shell->surface || !shell->xdg_surface || !shell->toplevel ||
        connection->api.proxy_add_listener((struct wl_proxy *)shell->xdg_surface,
                                           (void (**)(void))(void *)&g_vgfx_xdg_surface_listener,
                                           shell) != 0 ||
        connection->api.proxy_add_listener((struct wl_proxy *)shell->toplevel,
                                           (void (**)(void))(void *)&g_vgfx_xdg_toplevel_listener,
                                           shell) != 0) {
        vgfx_wayland_shell_close(shell);
        vgfx_wayland_shell_error(error, error_size, "could not create xdg toplevel objects");
        return 0;
    }

    if (connection->decoration_manager_v1) {
        shell->decoration =
            (struct zxdg_toplevel_decoration_v1 *)connection->api.proxy_marshal_flags(
                (struct wl_proxy *)connection->decoration_manager_v1,
                VGFX_ZXDG_DECORATION_MANAGER_GET_TOPLEVEL_DECORATION,
                &vgfx_zxdg_toplevel_decoration_v1_interface,
                1,
                0,
                NULL,
                shell->toplevel);
        if (shell->decoration &&
            connection->api.proxy_add_listener((struct wl_proxy *)shell->decoration,
                                               (void (**)(void))(void *)&g_vgfx_decoration_listener,
                                               shell) == 0) {
            (void)connection->api.proxy_marshal_flags(
                (struct wl_proxy *)shell->decoration,
                VGFX_ZXDG_TOPLEVEL_DECORATION_SET_MODE,
                NULL,
                1,
                0,
                VGFX_ZXDG_TOPLEVEL_DECORATION_MODE_SERVER_SIDE);
        } else if (shell->decoration) {
            connection->api.proxy_destroy((struct wl_proxy *)shell->decoration);
            shell->decoration = NULL;
        }
    }

    const char *safe_title = title && title[0] ? title : "Zanna";
    const char *safe_app_id = app_id && app_id[0] ? app_id : "org.zanna.app";
    (void)connection->api.proxy_marshal_flags((struct wl_proxy *)shell->toplevel,
                                              VGFX_XDG_TOPLEVEL_SET_TITLE,
                                              NULL,
                                              vgfx_wayland_proxy_version(shell, shell->toplevel),
                                              0,
                                              safe_title);
    (void)connection->api.proxy_marshal_flags((struct wl_proxy *)shell->toplevel,
                                              VGFX_XDG_TOPLEVEL_SET_APP_ID,
                                              NULL,
                                              vgfx_wayland_proxy_version(shell, shell->toplevel),
                                              0,
                                              safe_app_id);
    (void)connection->api.proxy_marshal_flags((struct wl_proxy *)shell->surface,
                                              VGFX_WL_SURFACE_COMMIT,
                                              NULL,
                                              vgfx_wayland_proxy_version(shell, shell->surface),
                                              0);
    if (connection->api.display_roundtrip(connection->display) < 0 || !shell->configured) {
        vgfx_wayland_shell_close(shell);
        vgfx_wayland_shell_error(error, error_size, "initial configure handshake failed");
        return 0;
    }
    return 1;
}
