//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_scale.h
// Purpose: Track Wayland outputs and integer framebuffer scaling for one surface.
// Key invariants: The active scale is the maximum scale of all entered outputs.
// Ownership/Lifetime: Owns bound wl_output proxies and borrows connection/shell.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "vgfx_wayland_shell.h"

/// @file
/// @brief Declares per-surface Wayland output tracking and framebuffer scaling.
/// @details Integer scaling follows the largest scale among outputs currently containing the
/// surface. When viewporter and fractional-scale-v1 are available, the module instead keeps
/// the surface buffer scale at one and exposes the compositor's scale in 1/120 increments.

/// @brief Tracked properties of one bound `wl_output` global.
typedef struct vgfx_wayland_output {
    /// @brief Owned output proxy, or NULL for an unused entry.
    struct wl_proxy *proxy;

    /// @brief Registry name used to match global-removal events.
    uint32_t name;

    /// @brief Positive integer buffer scale advertised by the output.
    int32_t scale;

    /// @brief Pixel width of the output's current mode.
    int32_t mode_width;

    /// @brief Pixel height of the output's current mode.
    int32_t mode_height;

    /// @brief Nonzero while the tracked surface overlaps this output.
    int32_t entered;
} vgfx_wayland_output_t;

/// @brief Scaling state and output inventory for one Wayland surface.
/// @details The object owns each proxy stored in @ref outputs, plus @ref viewport and
/// @ref fractional_scale when negotiated. The connection and shell are borrowed.
typedef struct vgfx_wayland_scale {
    /// @brief Borrowed Wayland connection and protocol dispatch state.
    vgfx_wayland_connection_t *connection;

    /// @brief Borrowed shell state for the surface being scaled.
    vgfx_wayland_shell_t *shell;

    /// @brief Fixed-capacity inventory of bound output globals.
    vgfx_wayland_output_t outputs[16];

    /// @brief Owned viewporter object for logical destination sizing, or NULL.
    struct wp_viewport *viewport;

    /// @brief Owned fractional-scale event object, or NULL.
    struct wp_fractional_scale_v1 *fractional_scale;

    /// @brief Number of initialized entries in @ref outputs.
    uint32_t output_count;

    /// @brief Current positive integer fallback buffer scale.
    int32_t scale;

    /// @brief Core-protocol preferred buffer scale, or zero when unavailable.
    int32_t preferred_buffer_scale;

    /// @brief Fractional preferred scale in units of 1/120, or zero when unknown.
    uint32_t preferred_scale_120;

    /// @brief Last logical viewport destination width sent to the compositor.
    int32_t logical_width;

    /// @brief Last logical viewport destination height sent to the compositor.
    int32_t logical_height;

    /// @brief Monotonic counter incremented whenever the effective scale changes.
    uint64_t generation;
} vgfx_wayland_scale_t;

/// @brief Begin output tracking and negotiate optional fractional scaling for a surface.
/// @details Installs connection and shell observers, binds all known outputs, then creates a
/// viewport/fractional-scale pair when both globals are available. Failure to negotiate the
/// optional pair falls back to integer scaling; a display round-trip failure fails the open.
/// @param scale Destination state, replaced with newly initialized state.
/// @param connection Borrowed open Wayland connection.
/// @param shell Borrowed shell state with a valid core surface.
/// @return 1 when the initial registry round trip succeeds, otherwise 0.
int vgfx_wayland_scale_open(vgfx_wayland_scale_t *scale,
                            vgfx_wayland_connection_t *connection,
                            vgfx_wayland_shell_t *shell);

/// @brief Detach observers, destroy owned proxies, and clear scaling state.
/// @param scale State to close, or NULL.
void vgfx_wayland_scale_close(vgfx_wayland_scale_t *scale);

/// @brief Return the effective framebuffer scale for the tracked surface.
/// @param scale Scaling state, or NULL for the default scale.
/// @return Fractional preferred scale when active, otherwise the positive integer scale;
/// returns 1.0 when no valid state is available.
float vgfx_wayland_scale_factor(const vgfx_wayland_scale_t *scale);

/// @brief Select the maximum valid scale among entered outputs (testable policy helper).
/// @param outputs Array of tracked outputs, or NULL.
/// @param count Number of entries available in @p outputs.
/// @return The greatest positive scale among entered outputs, or 1 when none qualify.
int32_t vgfx_wayland_scale_select_factor(const vgfx_wayland_output_t *outputs, uint32_t count);

/// @brief Publish the logical destination used by an optional wp_viewport.
/// @details Duplicate, non-positive, or unsupported destination sizes are ignored.
/// @param scale Scaling state whose viewport is updated.
/// @param width Positive logical surface width.
/// @param height Positive logical surface height.
void vgfx_wayland_scale_set_logical_size(vgfx_wayland_scale_t *scale,
                                         int32_t width,
                                         int32_t height);

/// @brief Query the current pixel dimensions of an output containing the surface.
/// @param scale Scaling state to inspect.
/// @param width Optional destination for the current mode width.
/// @param height Optional destination for the current mode height.
/// @return 1 when an entered output has a valid current mode, otherwise 0.
int vgfx_wayland_scale_monitor_size(const vgfx_wayland_scale_t *scale,
                                    int32_t *width,
                                    int32_t *height);
