//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_shell.h
// Purpose: Own the core and xdg-shell objects that give a Wayland window its role.
// Key invariants:
//   - A successful open has acknowledged its first xdg_surface configure.
//   - Destruction follows role object, xdg surface, then wl_surface order.
// Ownership/Lifetime:
//   - The shell surface borrows its connection and owns its three protocol proxies.
//   - Listener callbacks are valid until vgfx_wayland_shell_close() returns.
// Links: src/lib/graphics/src/vgfx_wayland_connection.h
//
//===----------------------------------------------------------------------===//

#pragma once

#include "vgfx_wayland_connection.h"

/// @file
/// @brief Declares ownership and observable state for one xdg-shell toplevel.
/// @details The shell object creates the core surface and its xdg role, tracks compositor
/// configure state, and forwards output membership and preferred-scale events to optional
/// observers.

/// @brief Core and xdg-shell protocol state for one Wayland window.
/// @details The structure owns @ref surface, @ref xdg_surface, @ref toplevel, and the optional
/// @ref decoration. The connection and observer contexts are borrowed.
typedef struct vgfx_wayland_shell {
    /// @brief Borrowed open connection providing globals and dispatch entry points.
    vgfx_wayland_connection_t *connection;

    /// @brief Owned core surface used for buffer attachment and commits.
    struct wl_surface *surface;

    /// @brief Owned xdg-shell surface role wrapper.
    struct xdg_surface *xdg_surface;

    /// @brief Owned xdg-shell toplevel window role.
    struct xdg_toplevel *toplevel;

    /// @brief Owned optional server-side decoration negotiation object.
    struct zxdg_toplevel_decoration_v1 *decoration;

    /// @brief Nonzero after the first `xdg_surface.configure` has been acknowledged.
    int32_t configured;

    /// @brief Nonzero after the compositor requests that the window close.
    int32_t close_requested;

    /// @brief Latest positive logical width proposed by the compositor.
    int32_t width;

    /// @brief Latest positive logical height proposed by the compositor.
    int32_t height;

    /// @brief Nonzero when the latest toplevel state array includes maximized.
    int32_t maximized;

    /// @brief Nonzero when the latest toplevel state array includes fullscreen.
    int32_t fullscreen;

    /// @brief Nonzero when the latest toplevel state array includes resizing.
    int32_t resizing;

    /// @brief Nonzero when the latest toplevel state array includes activated.
    int32_t activated;

    /// @brief Decoration mode most recently selected by the compositor.
    uint32_t decoration_mode;

    /// @brief Output proxies currently containing the surface.
    struct wl_proxy *entered_outputs[16];

    /// @brief Number of initialized entries in @ref entered_outputs.
    uint32_t entered_output_count;

    /// @brief Optional callback for surface entry into or exit from an output.
    void (*output_observer)(void *data, struct wl_proxy *output, int32_t entered);

    /// @brief Borrowed context passed to @ref output_observer.
    void *output_observer_data;

    /// @brief Optional callback for a core-protocol preferred integer buffer scale.
    void (*preferred_scale_observer)(void *data, int32_t factor);

    /// @brief Borrowed context passed to @ref preferred_scale_observer.
    void *preferred_scale_observer_data;
} vgfx_wayland_shell_t;

/// @brief Create an xdg toplevel and complete its initial configure handshake.
/// @details Creates the core surface, xdg surface, and toplevel role; installs all listeners;
/// optionally requests server-side decorations; assigns safe title and application-id
/// fallbacks; and performs the empty initial commit required by xdg-shell. Any failure destroys
/// all objects created by the attempt and clears @p shell.
/// @param shell Destination shell state.
/// @param connection Borrowed open connection with compositor and xdg-shell globals.
/// @param title Optional window title; NULL or empty selects `"Zanna"`.
/// @param app_id Optional desktop application id; NULL or empty selects `"org.zanna.app"`.
/// @param error Optional destination for a stable diagnostic.
/// @param error_size Capacity of @p error in bytes, including its terminator.
/// @return 1 after the initial configure has been received and acknowledged, otherwise 0.
int vgfx_wayland_shell_open(vgfx_wayland_shell_t *shell,
                            vgfx_wayland_connection_t *connection,
                            const char *title,
                            const char *app_id,
                            char *error,
                            uint32_t error_size);

/// @brief Destroy all owned role and surface objects and zero the shell.
/// @details Objects are destroyed in protocol dependency order: decoration, toplevel,
/// xdg surface, then core surface. Passing NULL is harmless.
/// @param shell Shell state to close, or NULL.
void vgfx_wayland_shell_close(vgfx_wayland_shell_t *shell);
