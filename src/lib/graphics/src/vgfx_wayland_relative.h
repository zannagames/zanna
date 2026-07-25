//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_relative.h
// Purpose: Provide native Wayland pointer lock and unbounded relative motion.
// Key invariants: Native mode requires both relative-pointer and pointer-constraints globals.
// Ownership/Lifetime: Owns per-pointer proxies and borrows connection, input, surface, and window.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "vgfx_wayland_input.h"

/// @file
/// @brief Declares native Wayland pointer locking and relative-motion collection.
/// @details The module combines `relative-pointer-v1` with `pointer-constraints-v1`. It reports
/// unaccelerated deltas to the owning graphics window while preserving the core Wayland
/// pointer object owned by the input subsystem.

/// @brief State for one window's relative pointer and persistent pointer lock.
/// @details The structure owns @ref relative_pointer and @ref locked_pointer. All other
/// pointers are borrowed and must outlive the enabled relative-pointer session.
typedef struct vgfx_wayland_relative {
    /// @brief Borrowed connection providing protocol globals and dispatch entry points.
    vgfx_wayland_connection_t *connection;

    /// @brief Borrowed input state providing the active core pointer proxy.
    vgfx_wayland_input_t *input;

    /// @brief Borrowed graphics window that receives accumulated relative deltas.
    struct vgfx_window *window;

    /// @brief Borrowed core surface whose pointer is constrained.
    struct wl_proxy *surface;

    /// @brief Owned relative-pointer-v1 event proxy, or NULL while disabled.
    struct zwp_relative_pointer_v1 *relative_pointer;

    /// @brief Owned persistent locked-pointer-v1 constraint, or NULL while disabled.
    struct zwp_locked_pointer_v1 *locked_pointer;

    /// @brief Nonzero after both extension proxies and their listeners are installed.
    int32_t enabled;

    /// @brief Nonzero while the compositor reports that the pointer constraint is active.
    int32_t locked;
} vgfx_wayland_relative_t;

/// @brief Initialize a disabled relative-pointer state with borrowed dependencies.
/// @details Any prior contents of @p relative are discarded without destroying proxies;
/// callers must invoke @ref vgfx_wayland_relative_close before reinitializing live state.
/// @param relative Destination state.
/// @param connection Borrowed Wayland connection.
/// @param input Borrowed core input state.
/// @param surface Borrowed core surface to constrain.
/// @param window Borrowed graphics window that receives deltas.
void vgfx_wayland_relative_init(vgfx_wayland_relative_t *relative,
                                vgfx_wayland_connection_t *connection,
                                vgfx_wayland_input_t *input,
                                struct wl_proxy *surface,
                                struct vgfx_window *window);

/// @brief Enable or disable native relative pointer input for a window.
/// @details Enabling creates both extension proxies and installs their listeners atomically.
/// Disabling destroys owned proxies, clears transient state, and restores the borrowed
/// dependencies so the object can be enabled again.
/// @param relative State to update.
/// @param enabled Nonzero to enable relative input; zero to disable it.
/// @return 1 when relative input is enabled, otherwise 0. A return of 0 includes normal
/// disablement and unsupported or failed enablement.
int vgfx_wayland_relative_set_enabled(vgfx_wayland_relative_t *relative, int32_t enabled);

/// @brief Destroy owned extension proxies and clear the relative-pointer state.
/// @param relative State to close, or NULL.
void vgfx_wayland_relative_close(vgfx_wayland_relative_t *relative);

/// @brief Convert a Wayland 24.8 fixed-point value to double precision.
/// @param value Wayland fixed-point value.
/// @return The equivalent floating-point value.
double vgfx_wayland_relative_fixed(wl_fixed_t value);
