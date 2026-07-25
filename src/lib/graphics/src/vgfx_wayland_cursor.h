//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_cursor.h
// Purpose: Manage themed Wayland cursor surfaces without link-time dependencies.
// Key invariants: Cursor updates use the most recent pointer-enter serial.
// Ownership/Lifetime: One cursor object owns its theme library, theme, and wl_surface.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Dynamically loaded Wayland cursor-theme integration.
/// @details Defines optional libwayland-cursor state that owns its library,
///          theme, and cursor surface while borrowing the connection and input
///          objects that provide protocol dispatch and pointer serials.

#pragma once

#include "vgfx_wayland_input.h"

struct wl_cursor_theme;
struct wl_cursor_image;
struct wl_cursor;

/// @brief State for one themed Wayland pointer cursor.
typedef struct vgfx_wayland_cursor {
    /// @brief Borrowed connection and input integration.
    vgfx_wayland_connection_t *connection;
    vgfx_wayland_input_t *input;
    /// @brief Owned optional cursor library, theme, and protocol surface.
    void *library;
    struct wl_cursor_theme *theme;
    struct wl_proxy *surface;
    /// @brief Dynamically resolved libwayland-cursor functions.
    struct wl_cursor_theme *(*theme_load)(const char *, int, struct wl_proxy *);
    void (*theme_destroy)(struct wl_cursor_theme *);
    struct wl_cursor *(*theme_get_cursor)(struct wl_cursor_theme *, const char *);
    struct wl_proxy *(*image_get_buffer)(struct wl_cursor_image *);
    /// @brief Requested public cursor type and visibility.
    int32_t type;
    int32_t visible;
} vgfx_wayland_cursor_t;

/// @brief Return the primary Xcursor theme name for one public cursor type.
/// @param type Public `VGFX_CURSOR_*` value.
/// @return Static Xcursor name, defaulting to `default`.
const char *vgfx_wayland_cursor_name(int32_t type);

/// @brief Initialize optional themed-cursor support.
/// @details Absence of libwayland-cursor or individual symbols is a supported
///          degradation and still returns success.  When resources are
///          available, loads the configured theme/size, creates a cursor
///          surface, and installs the pointer-enter callback.
/// @param cursor Destination cursor state.
/// @param connection Borrowed initialized Wayland connection.
/// @param input Borrowed input state.
/// @return 1 for valid arguments, including graceful feature unavailability;
///         otherwise 0.
int vgfx_wayland_cursor_open(vgfx_wayland_cursor_t *cursor,
                             vgfx_wayland_connection_t *connection,
                             vgfx_wayland_input_t *input);

/// @brief Remove callbacks and release all themed-cursor resources.
/// @param cursor Cursor state to close; NULL is ignored.
void vgfx_wayland_cursor_close(vgfx_wayland_cursor_t *cursor);

/// @brief Set cursor type and visibility using the latest pointer-enter serial.
/// @param cursor Initialized cursor state.
/// @param type Public `VGFX_CURSOR_*` value.
/// @param visible Non-zero to show, zero to hide.
void vgfx_wayland_cursor_set(vgfx_wayland_cursor_t *cursor, int32_t type, int32_t visible);
