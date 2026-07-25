//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_csd.h
// Purpose: Built-in Wayland client-side frame used when server decorations are unavailable.
// Key invariants:
//   - The application content surface and framebuffer dimensions remain unchanged.
//   - One synchronized subsurface supplies title-bar, border, and resize input regions.
// Ownership/Lifetime:
//   - The CSD object owns its child surface, subsurface role, SHM presenter, and RGBA source.
//   - It borrows the shell, input object, and ZannaGFX window.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Built-in Wayland client-side decoration frame.
/// @details Defines the synchronized subsurface, shared-memory presentation,
///          and pointer-routing state used when server-side decorations are
///          unavailable or explicitly overridden.

#pragma once

#include "vgfx_wayland_input.h"
#include "vgfx_wayland_shm.h"

/// @brief Owned fallback title bar and border state for one Wayland window.
typedef struct vgfx_wayland_csd {
    /// @brief Borrowed integration objects.
    vgfx_wayland_shell_t *shell;
    vgfx_wayland_input_t *input;
    struct vgfx_window *window;
    /// @brief Owned decoration surface, subsurface role, and SHM presenter.
    struct wl_surface *surface;
    struct wl_proxy *subsurface;
    vgfx_wayland_shm_presenter_t presenter;
    /// @brief Owned RGBA decoration source and byte size.
    uint8_t *pixels;
    size_t pixels_size;
    /// @brief Logical application-content dimensions enclosed by the frame.
    int32_t width;
    int32_t height;
    /// @brief Latest pointer position relative to the decoration surface.
    int32_t pointer_x;
    int32_t pointer_y;
    /// @brief Non-zero when fallback decorations are installed.
    int32_t active;
} vgfx_wayland_csd_t;

/// @brief Create the fallback frame when the compositor will not draw server decorations.
/// @details Returns success without allocating when server decorations are
///          active, unless `ZANNA_WAYLAND_FORCE_CSD=1`.  Otherwise creates and
///          positions a child subsurface, installs pointer routing, and draws
///          the initial frame.
/// @param csd Destination state.
/// @param shell Borrowed initialized xdg-shell state.
/// @param input Borrowed input state receiving the foreign-surface callback.
/// @param window Borrowed owning core window.
/// @param width Initial logical content width.
/// @param height Initial logical content height.
/// @param error Destination for a stable failure diagnostic.
/// @param error_size Capacity of @p error including its terminator.
/// @return One when active or unnecessary, zero only for a failed required fallback setup.
int vgfx_wayland_csd_open(vgfx_wayland_csd_t *csd,
                          vgfx_wayland_shell_t *shell,
                          vgfx_wayland_input_t *input,
                          struct vgfx_window *window,
                          int32_t width,
                          int32_t height,
                          char *error,
                          uint32_t error_size);

/// @brief Resize and redraw an active fallback frame; inactive objects are accepted.
/// @details Reallocates RGBA source storage, updates subsurface position and
///          xdg window geometry, recreates its SHM buffer, rasterizes controls,
///          and immediately presents the frame.
/// @param csd Decoration state to resize.
/// @param width New logical content width.
/// @param height New logical content height.
/// @param error Destination for a stable failure diagnostic.
/// @param error_size Capacity of @p error including its terminator.
/// @return 1 on success or for inactive state, otherwise 0.
int vgfx_wayland_csd_resize(vgfx_wayland_csd_t *csd,
                            int32_t width,
                            int32_t height,
                            char *error,
                            uint32_t error_size);

/// @brief Destroy all fallback-frame resources and clear installed input callbacks.
/// @param csd Decoration state to close; NULL is ignored.
void vgfx_wayland_csd_close(vgfx_wayland_csd_t *csd);
