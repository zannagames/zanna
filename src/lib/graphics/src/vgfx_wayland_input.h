//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_input.h
// Purpose: Translate core Wayland seat input into ZannaGFX events and state.
// Key invariants:
//   - Pointer coordinates are converted from wl_fixed_t without Wayland headers.
//   - Keyboard layout state is optional and dynamically loaded from xkbcommon.
// Ownership/Lifetime:
//   - An input object borrows its connection, surface, and ZannaGFX window.
//   - It owns pointer/keyboard proxies and all xkb objects it creates.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Wayland seat input translation and optional xkbcommon integration.
/// @details Defines pointer, keyboard, touch, repeat, modifier, and callback
///          state owned by one window's wl_seat binding.

#pragma once

#include "vgfx_wayland_connection.h"
#include "vgfx.h"

#include <stdint.h>

struct vgfx_window;

/// @brief Dynamically loaded subset of the xkbcommon API and owned objects.
typedef struct vgfx_wayland_xkb {
    /// @brief Owned library and xkb context/keymap/state objects.
    void *library;
    void *context;
    void *keymap;
    void *state;
    /// @brief Resolved xkbcommon entry points.
    void *(*context_new)(int flags);
    void (*context_unref)(void *context);
    void *(*keymap_new_from_string)(void *context, const char *string, int format, int flags);
    void (*keymap_unref)(void *keymap);
    void *(*state_new)(void *keymap);
    void (*state_unref)(void *state);
    int (*state_update_mask)(void *state,
                             uint32_t depressed,
                             uint32_t latched,
                             uint32_t locked,
                             uint32_t depressed_layout,
                             uint32_t latched_layout,
                             uint32_t locked_layout);
    uint32_t (*state_key_get_utf32)(void *state, uint32_t key);
    int (*state_mod_name_is_active)(void *state, const char *name, int type);
    int (*keymap_key_repeats)(void *keymap, uint32_t key);
} vgfx_wayland_xkb_t;

/// @brief Per-window Wayland input devices and translated state.
typedef struct vgfx_wayland_input {
    /// @brief Borrowed connection, application surface, and core window.
    vgfx_wayland_connection_t *connection;
    struct wl_proxy *surface;
    struct vgfx_window *window;
    /// @brief Owned seat device proxies and current pointer-enter surface.
    struct wl_proxy *pointer;
    struct wl_proxy *keyboard;
    struct wl_proxy *touch;
    struct wl_proxy *pointer_surface;
    /// @brief Optional owned keyboard-layout integration.
    vgfx_wayland_xkb_t xkb;
    /// @brief Seat capability bits and recent interaction serials.
    uint32_t capabilities;
    uint32_t pointer_serial;
    uint32_t keyboard_serial;
    /// @brief Latest logical pointer position and active modifier mask.
    int32_t pointer_x;
    int32_t pointer_y;
    int32_t modifiers;
    /// @brief Compositor-configured keyboard repeat state.
    int32_t repeat_rate;
    int32_t repeat_delay;
    uint32_t repeat_code;
    vgfx_key_t repeat_key;
    int64_t repeat_at_ms;
    int32_t repeat_active;
    /// @brief Borrowed cursor callback invoked on pointer enter.
    void (*pointer_entered)(void *data, uint32_t serial);
    void *pointer_entered_data;
    /// @brief Borrowed callback for pointer events targeting foreign child surfaces.
    void (*foreign_pointer_event)(void *data,
                                  struct wl_proxy *surface,
                                  uint32_t serial,
                                  uint32_t time,
                                  int32_t x,
                                  int32_t y,
                                  uint32_t button,
                                  int32_t pressed);
    void *foreign_pointer_data;
    /// @brief Fixed pool of active translated touch contacts.
    struct {
        int32_t id;
        int32_t x;
        int32_t y;
        float major;
        float minor;
        float orientation;
        int32_t active;
    } contacts[16];
} vgfx_wayland_input_t;

/// @brief Attach a seat listener and create devices advertised by the compositor.
/// @param input Destination state.
/// @param connection Borrowed initialized Wayland connection.
/// @param surface Borrowed application surface.
/// @param window Borrowed owning core window.
/// @param error Destination for a stable failure diagnostic.
/// @param error_size Capacity of @p error including its terminator.
/// @return 1 when the seat listener was installed, otherwise 0.
int vgfx_wayland_input_open(vgfx_wayland_input_t *input,
                            vgfx_wayland_connection_t *connection,
                            struct wl_proxy *surface,
                            struct vgfx_window *window,
                            char *error,
                            uint32_t error_size);

/// @brief Destroy device proxies and keyboard-layout state; accepts partial objects.
/// @param input Input state to close; NULL is ignored.
void vgfx_wayland_input_close(vgfx_wayland_input_t *input);

/// @brief Emit compositor-configured keyboard repeats due at @p now_ms.
/// @param input Input state containing the active repeat key.
/// @param now_ms Current monotonic time in milliseconds.
/// @return Number of repeat key events emitted, capped to prevent event storms.
int vgfx_wayland_input_tick(vgfx_wayland_input_t *input, int64_t now_ms);

/// @brief Clamp an event wait timeout so a pending key repeat is delivered on time.
/// @param input Input state to inspect.
/// @param requested_ms Caller-requested timeout; negative means unbounded.
/// @param now_ms Current monotonic time in milliseconds.
/// @return Timeout no later than the next repeat deadline.
int32_t vgfx_wayland_input_clamp_timeout(const vgfx_wayland_input_t *input,
                                         int32_t requested_ms,
                                         int64_t now_ms);

/// @brief Convert one evdev key code from wl_keyboard.key to a stable Zanna key.
/// @details Exposed to the backend test target; unknown and modifier-only codes return UNKNOWN.
/// @param key Evdev key code.
/// @return Public key value or `VGFX_KEY_UNKNOWN`.
vgfx_key_t vgfx_wayland_translate_evdev_key(uint32_t key);

/// @brief Replace the public key-state snapshot from a wl_keyboard.enter key array.
/// @param input Input state and core window to update.
/// @param keys Borrowed wl_array of pressed evdev codes.
void vgfx_wayland_input_sync_pressed(vgfx_wayland_input_t *input,
                                     const struct wl_array *keys);

/// @brief Round one signed 24.8 Wayland fixed coordinate to the nearest integer pixel.
/// @param value Signed Wayland fixed-point value.
/// @return Nearest integer pixel using symmetric half-away-from-zero rounding.
int32_t vgfx_wayland_fixed_to_pixel(wl_fixed_t value);
