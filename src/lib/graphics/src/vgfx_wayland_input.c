//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_input.c
// Purpose: Implement core wl_seat pointer and keyboard input for ZannaGFX.
// Key invariants:
//   - Listener layouts exactly match the stable core Wayland protocol ABI.
//   - Keymap file descriptors are mapped read-only and closed on every path.
// Ownership/Lifetime: See vgfx_wayland_input.h.
// Links: src/lib/graphics/src/vgfx_wayland_input.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Core Wayland seat, pointer, keyboard, touch, and repeat implementation.

#define _POSIX_C_SOURCE 200809L

#include "vgfx_wayland_input.h"

#include "vgfx_internal.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    WL_SEAT_GET_POINTER = 0,
    WL_SEAT_GET_KEYBOARD = 1,
    WL_SEAT_GET_TOUCH = 2,
    WL_SEAT_CAPABILITY_POINTER = 1,
    WL_SEAT_CAPABILITY_KEYBOARD = 2,
    WL_SEAT_CAPABILITY_TOUCH = 4,
    WL_POINTER_RELEASE = 1,
    WL_KEYBOARD_RELEASE = 0,
    WL_TOUCH_RELEASE = 0,
    WL_POINTER_BUTTON_STATE_RELEASED = 0,
    WL_POINTER_BUTTON_STATE_PRESSED = 1,
    WL_KEYBOARD_KEY_STATE_RELEASED = 0,
    WL_KEYBOARD_KEY_STATE_PRESSED = 1,
    WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 = 1,
    XKB_KEYMAP_FORMAT_TEXT_V1 = 1,
    XKB_STATE_MODS_EFFECTIVE = 1 << 3,
    BTN_LEFT = 0x110,
    BTN_RIGHT = 0x111,
    BTN_MIDDLE = 0x112,
};

/// @brief Local ABI for wl_seat listener callbacks.
typedef struct {
    void (*capabilities)(void *, struct wl_proxy *, uint32_t);
    void (*name)(void *, struct wl_proxy *, const char *);
} vgfx_wl_seat_listener_t;

/// @brief Local ABI for wl_pointer listener callbacks through protocol version 8.
typedef struct {
    void (*enter)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *, wl_fixed_t, wl_fixed_t);
    void (*leave)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *);
    void (*motion)(void *, struct wl_proxy *, uint32_t, wl_fixed_t, wl_fixed_t);
    void (*button)(void *, struct wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*axis)(void *, struct wl_proxy *, uint32_t, uint32_t, wl_fixed_t);
    void (*frame)(void *, struct wl_proxy *);
    void (*axis_source)(void *, struct wl_proxy *, uint32_t);
    void (*axis_stop)(void *, struct wl_proxy *, uint32_t, uint32_t);
    void (*axis_discrete)(void *, struct wl_proxy *, uint32_t, int32_t);
    void (*axis_value120)(void *, struct wl_proxy *, uint32_t, int32_t);
    void (*axis_relative_direction)(void *, struct wl_proxy *, uint32_t, uint32_t);
} vgfx_wl_pointer_listener_t;

/// @brief Local ABI for wl_keyboard listener callbacks.
typedef struct {
    void (*keymap)(void *, struct wl_proxy *, uint32_t, int32_t, uint32_t);
    void (*enter)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *, struct wl_array *);
    void (*leave)(void *, struct wl_proxy *, uint32_t, struct wl_proxy *);
    void (*key)(void *, struct wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*modifiers)(void *, struct wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*repeat_info)(void *, struct wl_proxy *, int32_t, int32_t);
} vgfx_wl_keyboard_listener_t;

/// @brief Local ABI for wl_touch listener callbacks.
typedef struct {
    void (*down)(void *, struct wl_proxy *, uint32_t, uint32_t, struct wl_proxy *, int32_t,
                 wl_fixed_t, wl_fixed_t);
    void (*up)(void *, struct wl_proxy *, uint32_t, uint32_t, int32_t);
    void (*motion)(void *, struct wl_proxy *, uint32_t, int32_t, wl_fixed_t, wl_fixed_t);
    void (*frame)(void *, struct wl_proxy *);
    void (*cancel)(void *, struct wl_proxy *);
    void (*shape)(void *, struct wl_proxy *, int32_t, wl_fixed_t, wl_fixed_t);
    void (*orientation)(void *, struct wl_proxy *, int32_t, wl_fixed_t);
} vgfx_wl_touch_listener_t;

/// @copydoc vgfx_wayland_fixed_to_pixel
int32_t vgfx_wayland_fixed_to_pixel(wl_fixed_t value) {
    return value >= 0 ? (value + 128) / 256 : (value - 128) / 256;
}

/// @brief Normalize a Wayland event timestamp to the backend monotonic clock.
/// @param time Native millisecond timestamp, where zero means unavailable.
/// @return @p time when non-zero, otherwise `vgfx_platform_now_ms()`.
static int64_t vgfx_wl_time(uint32_t time) {
    return time ? (int64_t)time : vgfx_platform_now_ms();
}

/// @copydoc vgfx_wayland_translate_evdev_key
vgfx_key_t vgfx_wayland_translate_evdev_key(uint32_t key) {
    static const vgfx_key_t letters[26] = {VGFX_KEY_A, VGFX_KEY_S, VGFX_KEY_D, VGFX_KEY_F,
                                           VGFX_KEY_G, VGFX_KEY_H, VGFX_KEY_J, VGFX_KEY_K,
                                           VGFX_KEY_L, VGFX_KEY_UNKNOWN, VGFX_KEY_UNKNOWN,
                                           VGFX_KEY_UNKNOWN, VGFX_KEY_Z, VGFX_KEY_X,
                                           VGFX_KEY_C, VGFX_KEY_V, VGFX_KEY_B, VGFX_KEY_N,
                                           VGFX_KEY_M};
    if (key >= 30 && key <= 38)
        return letters[key - 30];
    if (key >= 44 && key <= 50)
        return letters[key - 32];
    if (key >= 2 && key <= 10)
        return (vgfx_key_t)(VGFX_KEY_1 + key - 2);
    if (key == 11)
        return VGFX_KEY_0;
    switch (key) {
    case 1: return VGFX_KEY_ESCAPE;
    case 14: return VGFX_KEY_BACKSPACE;
    case 15: return VGFX_KEY_TAB;
    case 16: return VGFX_KEY_Q;
    case 17: return VGFX_KEY_W;
    case 18: return VGFX_KEY_E;
    case 19: return VGFX_KEY_R;
    case 20: return VGFX_KEY_T;
    case 21: return VGFX_KEY_Y;
    case 22: return VGFX_KEY_U;
    case 23: return VGFX_KEY_I;
    case 24: return VGFX_KEY_O;
    case 25: return VGFX_KEY_P;
    case 28: return VGFX_KEY_ENTER;
    case 39: return VGFX_KEY_UNKNOWN;
    case 40: return VGFX_KEY_UNKNOWN;
    case 41: return VGFX_KEY_UNKNOWN;
    case 51: return VGFX_KEY_UNKNOWN;
    case 52: return VGFX_KEY_UNKNOWN;
    case 53: return VGFX_KEY_UNKNOWN;
    case 57: return VGFX_KEY_SPACE;
    /* Function keys: evdev KEY_F1..KEY_F10 are 59..68; F11/F12 are 87/88. */
    case 59: return VGFX_KEY_F1;
    case 60: return VGFX_KEY_F2;
    case 61: return VGFX_KEY_F3;
    case 62: return VGFX_KEY_F4;
    case 63: return VGFX_KEY_F5;
    case 64: return VGFX_KEY_F6;
    case 65: return VGFX_KEY_F7;
    case 66: return VGFX_KEY_F8;
    case 67: return VGFX_KEY_F9;
    case 68: return VGFX_KEY_F10;
    case 87: return VGFX_KEY_F11;
    case 88: return VGFX_KEY_F12;
    case 102: return VGFX_KEY_HOME;
    case 103: return VGFX_KEY_UP;
    case 104: return VGFX_KEY_PAGE_UP;
    case 105: return VGFX_KEY_LEFT;
    case 106: return VGFX_KEY_RIGHT;
    case 107: return VGFX_KEY_END;
    case 108: return VGFX_KEY_DOWN;
    case 109: return VGFX_KEY_PAGE_DOWN;
    case 111: return VGFX_KEY_DELETE;
    default: return VGFX_KEY_UNKNOWN;
    }
}

/// @brief Release all optional xkbcommon objects and unload its library.
/// @param xkb Keyboard-layout state to clear.
static void vgfx_wl_xkb_close(vgfx_wayland_xkb_t *xkb) {
    if (xkb->state && xkb->state_unref) xkb->state_unref(xkb->state);
    if (xkb->keymap && xkb->keymap_unref) xkb->keymap_unref(xkb->keymap);
    if (xkb->context && xkb->context_unref) xkb->context_unref(xkb->context);
    if (xkb->library) dlclose(xkb->library);
    memset(xkb, 0, sizeof(*xkb));
}

/// @brief Lazily load the required xkbcommon API and create a context.
/// @param xkb Destination dynamic keyboard-layout state.
/// @return 1 when all symbols and the context are available, otherwise 0.
static int vgfx_wl_xkb_load(vgfx_wayland_xkb_t *xkb) {
    if (xkb->library)
        return 1;
    xkb->library = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!xkb->library)
        return 0;
#define LOAD(name)                                                                                 \
    do {                                                                                           \
        *(void **)(&xkb->name) = dlsym(xkb->library, "xkb_" #name);                               \
        if (!xkb->name) {                                                                          \
            vgfx_wl_xkb_close(xkb);                                                                \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)
    LOAD(context_new);
    LOAD(context_unref);
    LOAD(keymap_new_from_string);
    LOAD(keymap_unref);
    LOAD(state_new);
    LOAD(state_unref);
    LOAD(state_update_mask);
    LOAD(state_key_get_utf32);
    LOAD(state_mod_name_is_active);
    LOAD(keymap_key_repeats);
#undef LOAD
    xkb->context = xkb->context_new(0);
    return xkb->context != NULL;
}

/// @brief Derive public modifier flags from current xkb state.
/// @param input Input state containing layout and fallback modifier state.
/// @return Bitwise Shift, Control, and Alt `VGFX_MOD_*` flags.
static int vgfx_wl_modifiers(vgfx_wayland_input_t *input) {
    if (!input->xkb.state || !input->xkb.state_mod_name_is_active)
        return input->modifiers;
    int result = 0;
    if (input->xkb.state_mod_name_is_active(input->xkb.state, "Shift", XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= VGFX_MOD_SHIFT;
    if (input->xkb.state_mod_name_is_active(input->xkb.state, "Control", XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= VGFX_MOD_CTRL;
    if (input->xkb.state_mod_name_is_active(input->xkb.state, "Mod1", XKB_STATE_MODS_EFFECTIVE) > 0)
        result |= VGFX_MOD_ALT;
    return result;
}

/// @brief Publish a pointer position and coalescible mouse-move event.
/// @param input Input state and target core window.
/// @param sx Wayland fixed-point surface X coordinate.
/// @param sy Wayland fixed-point surface Y coordinate.
/// @param time Native event timestamp.
static void vgfx_wl_pointer_position(vgfx_wayland_input_t *input, wl_fixed_t sx, wl_fixed_t sy, uint32_t time) {
    input->pointer_x = vgfx_wayland_fixed_to_pixel(sx);
    input->pointer_y = vgfx_wayland_fixed_to_pixel(sy);
    vgfx_internal_set_mouse_position(input->window, input->pointer_x, input->pointer_y);
    vgfx_event_t event = {.type = VGFX_EVENT_MOUSE_MOVE, .time_ms = vgfx_wl_time(time)};
    event.data.mouse_move.x = input->pointer_x;
    event.data.mouse_move.y = input->pointer_y;
    event.data.mouse_move.modifiers = input->modifiers;
    vgfx_internal_enqueue_coalesced_event(input->window, &event);
}

/// @brief Track pointer entry and route it to the application or foreign surface.
/// @param data Borrowed input context.
/// @param pointer Pointer proxy; unused.
/// @param serial Pointer-enter serial.
/// @param surface Entered surface.
/// @param sx Surface X coordinate.
/// @param sy Surface Y coordinate.
static void vgfx_wl_pointer_enter(void *data, struct wl_proxy *pointer, uint32_t serial,
                                  struct wl_proxy *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    vgfx_wayland_input_t *input = data;
    if (input) {
        input->pointer_surface = surface;
        input->pointer_serial = serial;
    }
    if (input && surface == input->surface) {
        vgfx_wl_pointer_position(input, sx, sy, 0);
        if (input->pointer_entered)
            input->pointer_entered(input->pointer_entered_data, serial);
    } else if (input && input->foreign_pointer_event) {
        input->foreign_pointer_event(input->foreign_pointer_data,
                                     surface,
                                     serial,
                                     0,
                                     vgfx_wayland_fixed_to_pixel(sx),
                                     vgfx_wayland_fixed_to_pixel(sy),
                                     0,
                                     0);
    }
}

/// @brief Clear the active pointer surface on leave.
/// @param data Borrowed input context.
/// @param pointer Pointer proxy; unused.
/// @param serial Pointer-leave serial.
/// @param surface Departed surface; unused.
static void vgfx_wl_pointer_leave(void *data, struct wl_proxy *pointer, uint32_t serial,
                                  struct wl_proxy *surface) {
    (void)pointer; (void)surface;
    vgfx_wayland_input_t *input = data;
    if (input) {
        input->pointer_serial = serial;
        input->pointer_surface = NULL;
    }
}

/// @brief Route pointer motion to application or foreign-surface handling.
/// @param data Borrowed input context.
/// @param pointer Pointer proxy; unused.
/// @param time Native timestamp.
/// @param sx Surface X coordinate.
/// @param sy Surface Y coordinate.
static void vgfx_wl_pointer_motion(void *data, struct wl_proxy *pointer, uint32_t time,
                                   wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    vgfx_wayland_input_t *input = data;
    if (!input)
        return;
    if (input->pointer_surface == input->surface) {
        vgfx_wl_pointer_position(input, sx, sy, time);
    } else if (input->foreign_pointer_event) {
        input->pointer_x = vgfx_wayland_fixed_to_pixel(sx);
        input->pointer_y = vgfx_wayland_fixed_to_pixel(sy);
        input->foreign_pointer_event(input->foreign_pointer_data,
                                     input->pointer_surface,
                                     input->pointer_serial,
                                     time,
                                     input->pointer_x,
                                     input->pointer_y,
                                     0,
                                     0);
    }
}

/// @brief Translate a native pointer button event or route it to a foreign surface.
/// @param data Borrowed input context.
/// @param pointer Pointer proxy; unused.
/// @param serial Button serial.
/// @param time Native timestamp.
/// @param code Linux input button code.
/// @param state Native pressed/released state.
static void vgfx_wl_pointer_button(void *data, struct wl_proxy *pointer, uint32_t serial,
                                   uint32_t time, uint32_t code, uint32_t state) {
    (void)pointer;
    vgfx_wayland_input_t *input = data;
    if (!input) return;
    input->pointer_serial = serial;
    int down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (input->pointer_surface != input->surface) {
        if (input->foreign_pointer_event)
            input->foreign_pointer_event(input->foreign_pointer_data,
                                         input->pointer_surface,
                                         serial,
                                         time,
                                         input->pointer_x,
                                         input->pointer_y,
                                         code,
                                         down);
        return;
    }
    vgfx_mouse_button_t button;
    if (code == BTN_LEFT) button = VGFX_MOUSE_LEFT;
    else if (code == BTN_RIGHT) button = VGFX_MOUSE_RIGHT;
    else if (code == BTN_MIDDLE) button = VGFX_MOUSE_MIDDLE;
    else return;
    vgfx_internal_set_mouse_button_state(input->window, button, down);
    vgfx_event_t event = {.type = down ? VGFX_EVENT_MOUSE_DOWN : VGFX_EVENT_MOUSE_UP,
                          .time_ms = vgfx_wl_time(time)};
    event.data.mouse_button.x = input->pointer_x;
    event.data.mouse_button.y = input->pointer_y;
    event.data.mouse_button.button = button;
    event.data.mouse_button.modifiers = input->modifiers;
    vgfx_internal_enqueue_event(input->window, &event);
}

/// @brief Translate a Wayland pointer axis value into a scroll event.
/// @param data Borrowed input context.
/// @param pointer Pointer proxy; unused.
/// @param time Native timestamp.
/// @param axis Zero for vertical, non-zero for horizontal.
/// @param value Signed 24.8 axis distance.
static void vgfx_wl_pointer_axis(void *data, struct wl_proxy *pointer, uint32_t time,
                                 uint32_t axis, wl_fixed_t value) {
    (void)pointer;
    vgfx_wayland_input_t *input = data;
    if (!input) return;
    vgfx_event_t event = {.type = VGFX_EVENT_SCROLL, .time_ms = vgfx_wl_time(time)};
    if (axis == 0) event.data.scroll.delta_y = (float)value / 2560.0f;
    else event.data.scroll.delta_x = (float)value / 2560.0f;
    event.data.scroll.x = input->pointer_x;
    event.data.scroll.y = input->pointer_y;
    event.data.scroll.modifiers = input->modifiers;
    vgfx_internal_enqueue_event(input->window, &event);
}

/// @brief Ignore the pointer frame delimiter because events are published immediately.
/// @param d Borrowed input-listener context; intentionally unused.
/// @param p Pointer proxy that emitted the frame event; intentionally unused.
static void vgfx_wl_pointer_frame(void *d, struct wl_proxy *p) { (void)d; (void)p; }
/// @brief Ignore optional pointer axis-source metadata.
/// @param d Borrowed input-listener context; intentionally unused.
/// @param p Pointer proxy that emitted the metadata; intentionally unused.
/// @param s Compositor-reported axis source; intentionally unused.
static void vgfx_wl_pointer_axis_source(void *d, struct wl_proxy *p, uint32_t s) { (void)d; (void)p; (void)s; }
/// @brief Ignore optional pointer axis-stop metadata.
/// @param d Borrowed input-listener context; intentionally unused.
/// @param p Pointer proxy that emitted the metadata; intentionally unused.
/// @param t Event timestamp in compositor milliseconds; intentionally unused.
/// @param a Axis whose scrolling stopped; intentionally unused.
static void vgfx_wl_pointer_axis_stop(void *d, struct wl_proxy *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
/// @brief Ignore optional discrete-axis metadata in favor of continuous values.
/// @param d Borrowed input-listener context; intentionally unused.
/// @param p Pointer proxy that emitted the metadata; intentionally unused.
/// @param a Axis associated with the discrete step count; intentionally unused.
/// @param v Discrete scroll-step count; intentionally unused.
static void vgfx_wl_pointer_axis_discrete(void *d, struct wl_proxy *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
/// @brief Ignore optional axis-value120 metadata in favor of continuous values.
/// @param d Borrowed input-listener context; intentionally unused.
/// @param p Pointer proxy that emitted the metadata; intentionally unused.
/// @param a Axis associated with the high-resolution delta; intentionally unused.
/// @param v Scroll delta expressed in 120ths of a logical step; intentionally unused.
static void vgfx_wl_pointer_axis_value120(void *d, struct wl_proxy *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
/// @brief Ignore optional natural-scroll direction metadata.
/// @param d Borrowed input-listener context; intentionally unused.
/// @param p Pointer proxy that emitted the metadata; intentionally unused.
/// @param a Axis whose direction is described; intentionally unused.
/// @param v Compositor-provided direction value; intentionally unused.
static void vgfx_wl_pointer_axis_direction(void *d, struct wl_proxy *p, uint32_t a, uint32_t v) { (void)d; (void)p; (void)a; (void)v; }

static const vgfx_wl_pointer_listener_t g_pointer_listener = {
    vgfx_wl_pointer_enter, vgfx_wl_pointer_leave, vgfx_wl_pointer_motion,
    vgfx_wl_pointer_button, vgfx_wl_pointer_axis, vgfx_wl_pointer_frame,
    vgfx_wl_pointer_axis_source, vgfx_wl_pointer_axis_stop, vgfx_wl_pointer_axis_discrete,
    vgfx_wl_pointer_axis_value120, vgfx_wl_pointer_axis_direction};

/// @brief Adopt a compositor-provided xkb keymap from a read-only file mapping.
/// @param data Borrowed input context.
/// @param keyboard Keyboard proxy; unused.
/// @param format Native keymap format.
/// @param fd Owned keymap descriptor, closed on every path.
/// @param size Mapped keymap byte count.
static void vgfx_wl_keyboard_keymap(void *data, struct wl_proxy *keyboard, uint32_t format,
                                    int32_t fd, uint32_t size) {
    (void)keyboard;
    vgfx_wayland_input_t *input = data;
    if (!input || format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) { close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED || !vgfx_wl_xkb_load(&input->xkb)) {
        if (map != MAP_FAILED) munmap(map, size);
        return;
    }
    if (input->xkb.state) input->xkb.state_unref(input->xkb.state);
    if (input->xkb.keymap) input->xkb.keymap_unref(input->xkb.keymap);
    input->xkb.state = NULL;
    input->xkb.keymap = input->xkb.keymap_new_from_string(
        input->xkb.context, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map, size);
    if (input->xkb.keymap) input->xkb.state = input->xkb.state_new(input->xkb.keymap);
}

/// @copydoc vgfx_wayland_input_sync_pressed
void vgfx_wayland_input_sync_pressed(vgfx_wayland_input_t *input,
                                     const struct wl_array *keys) {
    if (!input || !input->window)
        return;
    vgfx_internal_clear_input_state(input->window);
    if (!keys || !keys->data)
        return;
    const uint32_t *pressed = (const uint32_t *)keys->data;
    size_t count = keys->size / sizeof(*pressed);
    for (size_t i = 0; i < count; ++i) {
        vgfx_key_t key = vgfx_wayland_translate_evdev_key(pressed[i]);
        if (key != VGFX_KEY_UNKNOWN)
            vgfx_internal_set_key_state(input->window, key, 1);
    }
}

/// @brief Record keyboard focus entry and synchronize pressed keys.
/// @param data Borrowed input context.
/// @param keyboard Keyboard proxy; unused.
/// @param serial Keyboard-enter serial.
/// @param surface Entered surface.
/// @param keys Borrowed array of pressed evdev codes.
static void vgfx_wl_keyboard_enter(void *data, struct wl_proxy *keyboard, uint32_t serial,
                                   struct wl_proxy *surface, struct wl_array *keys) {
    (void)keyboard;
    vgfx_wayland_input_t *input = data;
    if (!input || surface != input->surface)
        return;
    input->keyboard_serial = serial;
    vgfx_wayland_input_sync_pressed(input, keys);
}

/// @brief Reset keyboard state when focus leaves the application surface.
/// @param data Borrowed input context.
/// @param keyboard Keyboard proxy; unused.
/// @param serial Keyboard-leave serial.
/// @param surface Departed surface; unused.
static void vgfx_wl_keyboard_leave(void *data, struct wl_proxy *keyboard, uint32_t serial,
                                   struct wl_proxy *surface) {
    (void)keyboard; (void)surface;
    vgfx_wayland_input_t *input = data;
    if (input) {
        input->keyboard_serial = serial;
        input->repeat_active = 0;
        input->modifiers = 0;
        vgfx_internal_clear_input_state(input->window);
    }
}

/// @brief Emit key state and layout-derived text for one native key action.
/// @param input Input state and target window.
/// @param code Evdev key code.
/// @param key Translated public key.
/// @param down Non-zero for press.
/// @param repeat Non-zero for repeated press.
/// @param time_ms Event timestamp.
static void vgfx_wl_emit_key(vgfx_wayland_input_t *input,
                             uint32_t code,
                             vgfx_key_t key,
                             int down,
                             int repeat,
                             int64_t time_ms) {
    if (key != VGFX_KEY_UNKNOWN) {
        vgfx_internal_set_key_state(input->window, key, down);
        vgfx_event_t event = {.type = down ? VGFX_EVENT_KEY_DOWN : VGFX_EVENT_KEY_UP,
                              .time_ms = time_ms};
        event.data.key.key = key;
        event.data.key.is_repeat = repeat;
        event.data.key.modifiers = input->modifiers;
        vgfx_internal_enqueue_event(input->window, &event);
    }
    if (down && input->xkb.state && input->xkb.state_key_get_utf32) {
        uint32_t cp = input->xkb.state_key_get_utf32(input->xkb.state, code + 8);
        if (vgfx_internal_codepoint_is_text(cp) &&
            vgfx_internal_text_modifiers_allow_text(input->modifiers)) {
            vgfx_event_t event = {.type = VGFX_EVENT_TEXT_INPUT, .time_ms = time_ms};
            event.data.text.codepoint = cp;
            event.data.text.modifiers = input->modifiers;
            vgfx_internal_enqueue_event(input->window, &event);
        }
    }
}

/// @brief Translate a wl_keyboard key event and manage repeat scheduling.
/// @param data Borrowed input context.
/// @param keyboard Keyboard proxy; unused.
/// @param serial Key-event serial.
/// @param time Native timestamp.
/// @param code Evdev key code.
/// @param state Native pressed/released state.
static void vgfx_wl_keyboard_key(void *data, struct wl_proxy *keyboard, uint32_t serial,
                                 uint32_t time, uint32_t code, uint32_t state) {
    (void)keyboard;
    vgfx_wayland_input_t *input = data;
    if (!input) return;
    input->keyboard_serial = serial;
    int down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    vgfx_key_t key = vgfx_wayland_translate_evdev_key(code);
    int repeat = down && key != VGFX_KEY_UNKNOWN && vgfx_key_down(input->window, key);
    vgfx_wl_emit_key(input, code, key, down, repeat, vgfx_wl_time(time));
    if (down && input->repeat_rate > 0 &&
        ((!input->xkb.keymap || !input->xkb.keymap_key_repeats) ||
         input->xkb.keymap_key_repeats(input->xkb.keymap, code + 8) > 0)) {
        input->repeat_code = code;
        input->repeat_key = key;
        input->repeat_at_ms = vgfx_platform_now_ms() + input->repeat_delay;
        input->repeat_active = 1;
    } else if (!down && input->repeat_active && input->repeat_code == code) {
        input->repeat_active = 0;
    }
}

/// @brief Update xkb modifier state from compositor masks.
/// @param data Borrowed input context.
/// @param keyboard Keyboard proxy; unused.
/// @param serial Modifier-event serial.
/// @param depressed Depressed modifier mask.
/// @param latched Latched modifier mask.
/// @param locked Locked modifier mask.
/// @param group Active layout group.
static void vgfx_wl_keyboard_modifiers(void *data, struct wl_proxy *keyboard, uint32_t serial,
                                       uint32_t depressed, uint32_t latched, uint32_t locked,
                                       uint32_t group) {
    (void)keyboard;
    vgfx_wayland_input_t *input = data;
    if (!input) return;
    input->keyboard_serial = serial;
    if (input->xkb.state)
        input->xkb.state_update_mask(input->xkb.state, depressed, latched, locked, 0, 0, group);
    input->modifiers = vgfx_wl_modifiers(input);
}

/// @brief Store compositor keyboard-repeat rate and delay.
/// @param data Borrowed input context.
/// @param keyboard Keyboard proxy; unused.
/// @param rate Repeats per second; non-positive disables repeat.
/// @param delay Initial delay in milliseconds.
static void vgfx_wl_keyboard_repeat(void *data, struct wl_proxy *keyboard, int32_t rate, int32_t delay) {
    (void)keyboard;
    vgfx_wayland_input_t *input = data;
    if (input) {
        input->repeat_rate = rate > 0 ? rate : 0;
        input->repeat_delay = delay >= 0 ? delay : 0;
        if (input->repeat_rate == 0)
            input->repeat_active = 0;
    }
}
static const vgfx_wl_keyboard_listener_t g_keyboard_listener = {
    vgfx_wl_keyboard_keymap, vgfx_wl_keyboard_enter, vgfx_wl_keyboard_leave,
    vgfx_wl_keyboard_key, vgfx_wl_keyboard_modifiers, vgfx_wl_keyboard_repeat};

/// @brief Find or allocate a slot for a compositor touch identifier.
/// @param input Input state containing the fixed contact pool.
/// @param id Native touch identifier.
/// @param create Non-zero to allocate a free slot when absent.
/// @return Slot index in [0, 15], or -1.
static int vgfx_wl_touch_slot(vgfx_wayland_input_t *input, int32_t id, int create) {
    int free_slot = -1;
    for (int i = 0; i < 16; ++i) {
        if (input->contacts[i].active && input->contacts[i].id == id)
            return i;
        if (!input->contacts[i].active && free_slot < 0)
            free_slot = i;
    }
    if (create && free_slot >= 0) {
        memset(&input->contacts[free_slot], 0, sizeof(input->contacts[free_slot]));
        input->contacts[free_slot].id = id;
        input->contacts[free_slot].active = 1;
    }
    return create ? free_slot : -1;
}

/// @brief Publish one touch event from a cached contact slot.
/// @param input Input state and target window.
/// @param slot Contact slot index.
/// @param type Touch down, move, or up event type.
/// @param time Native timestamp.
static void vgfx_wl_touch_event(vgfx_wayland_input_t *input,
                                int slot,
                                vgfx_event_type_t type,
                                uint32_t time) {
    if (!input || slot < 0 || slot >= 16)
        return;
    vgfx_event_t event = {.type = type, .time_ms = vgfx_wl_time(time)};
    event.data.touch.id = input->contacts[slot].id;
    event.data.touch.x = input->contacts[slot].x;
    event.data.touch.y = input->contacts[slot].y;
    event.data.touch.major = input->contacts[slot].major;
    event.data.touch.minor = input->contacts[slot].minor;
    event.data.touch.orientation = input->contacts[slot].orientation;
    vgfx_internal_enqueue_event(input->window, &event);
}

/// @brief Allocate and publish a touch contact entering the application surface.
/// @param data Borrowed input context.
/// @param touch Touch proxy; unused.
/// @param serial Touch-down serial.
/// @param time Native timestamp.
/// @param surface Target surface.
/// @param id Native contact identifier.
/// @param x Surface X coordinate.
/// @param y Surface Y coordinate.
static void vgfx_wl_touch_down(void *data,
                               struct wl_proxy *touch,
                               uint32_t serial,
                               uint32_t time,
                               struct wl_proxy *surface,
                               int32_t id,
                               wl_fixed_t x,
                               wl_fixed_t y) {
    (void)touch;
    vgfx_wayland_input_t *input = data;
    if (!input || surface != input->surface)
        return;
    input->pointer_serial = serial;
    int slot = vgfx_wl_touch_slot(input, id, 1);
    if (slot >= 0) {
        input->contacts[slot].x = vgfx_wayland_fixed_to_pixel(x);
        input->contacts[slot].y = vgfx_wayland_fixed_to_pixel(y);
        vgfx_wl_touch_event(input, slot, VGFX_EVENT_TOUCH_DOWN, time);
    }
}

/// @brief Publish touch-up and release the matching contact slot.
/// @param data Borrowed input context.
/// @param touch Touch proxy; unused.
/// @param serial Touch-up serial.
/// @param time Native timestamp.
/// @param id Native contact identifier.
static void vgfx_wl_touch_up(void *data,
                             struct wl_proxy *touch,
                             uint32_t serial,
                             uint32_t time,
                             int32_t id) {
    (void)touch;
    vgfx_wayland_input_t *input = data;
    if (!input)
        return;
    input->pointer_serial = serial;
    int slot = vgfx_wl_touch_slot(input, id, 0);
    if (slot >= 0) {
        vgfx_wl_touch_event(input, slot, VGFX_EVENT_TOUCH_UP, time);
        input->contacts[slot].active = 0;
    }
}

/// @brief Update and publish motion for an active touch contact.
/// @param data Borrowed input context.
/// @param touch Touch proxy; unused.
/// @param time Native timestamp.
/// @param id Native contact identifier.
/// @param x Surface X coordinate.
/// @param y Surface Y coordinate.
static void vgfx_wl_touch_motion(void *data,
                                 struct wl_proxy *touch,
                                 uint32_t time,
                                 int32_t id,
                                 wl_fixed_t x,
                                 wl_fixed_t y) {
    (void)touch;
    vgfx_wayland_input_t *input = data;
    int slot = input ? vgfx_wl_touch_slot(input, id, 0) : -1;
    if (slot >= 0) {
        input->contacts[slot].x = vgfx_wayland_fixed_to_pixel(x);
        input->contacts[slot].y = vgfx_wayland_fixed_to_pixel(y);
        vgfx_wl_touch_event(input, slot, VGFX_EVENT_TOUCH_MOVE, time);
    }
}

/// @brief Ignore the touch frame delimiter because contacts publish immediately.
/// @param data Unused input context.
/// @param touch Unused touch proxy.
static void vgfx_wl_touch_frame(void *data, struct wl_proxy *touch) {
    (void)data;
    (void)touch;
}

/// @brief Cancel all active touch contacts and publish one cancel event.
/// @param data Borrowed input context.
/// @param touch Touch proxy; unused.
static void vgfx_wl_touch_cancel(void *data, struct wl_proxy *touch) {
    (void)touch;
    vgfx_wayland_input_t *input = data;
    if (!input)
        return;
    vgfx_event_t event = {.type = VGFX_EVENT_TOUCH_CANCEL, .time_ms = vgfx_platform_now_ms()};
    vgfx_internal_enqueue_event(input->window, &event);
    memset(input->contacts, 0, sizeof(input->contacts));
}

/// @brief Cache major/minor ellipse dimensions for an active touch contact.
/// @param data Borrowed input context.
/// @param touch Touch proxy; unused.
/// @param id Native contact identifier.
/// @param major Major-axis length in Wayland fixed units.
/// @param minor Minor-axis length in Wayland fixed units.
static void vgfx_wl_touch_shape(void *data,
                                struct wl_proxy *touch,
                                int32_t id,
                                wl_fixed_t major,
                                wl_fixed_t minor) {
    (void)touch;
    vgfx_wayland_input_t *input = data;
    int slot = input ? vgfx_wl_touch_slot(input, id, 0) : -1;
    if (slot >= 0) {
        input->contacts[slot].major = (float)major / 256.0f;
        input->contacts[slot].minor = (float)minor / 256.0f;
    }
}

/// @brief Cache orientation for an active touch contact.
/// @param data Borrowed input context.
/// @param touch Touch proxy; unused.
/// @param id Native contact identifier.
/// @param orientation Orientation in Wayland fixed units.
static void vgfx_wl_touch_orientation(void *data,
                                      struct wl_proxy *touch,
                                      int32_t id,
                                      wl_fixed_t orientation) {
    (void)touch;
    vgfx_wayland_input_t *input = data;
    int slot = input ? vgfx_wl_touch_slot(input, id, 0) : -1;
    if (slot >= 0)
        input->contacts[slot].orientation = (float)orientation / 256.0f;
}

static const vgfx_wl_touch_listener_t g_touch_listener = {
    vgfx_wl_touch_down,  vgfx_wl_touch_up,     vgfx_wl_touch_motion,
    vgfx_wl_touch_frame, vgfx_wl_touch_cancel, vgfx_wl_touch_shape,
    vgfx_wl_touch_orientation};

/// @brief Release an owned seat device proxy using its versioned destructor.
/// @param input Input state supplying the connection API.
/// @param proxy Address of the owned proxy, cleared on return.
/// @param opcode Protocol-specific release opcode.
static void vgfx_wl_release(vgfx_wayland_input_t *input, struct wl_proxy **proxy, uint32_t opcode) {
    if (!*proxy) return;
    if (input->connection && input->connection->api.proxy_get_version(*proxy) >= 3)
        (void)input->connection->api.proxy_marshal_flags(*proxy, opcode, NULL,
            input->connection->api.proxy_get_version(*proxy), 1u);
    else if (input->connection) input->connection->api.proxy_destroy(*proxy);
    *proxy = NULL;
}

/// @brief Reconcile owned device proxies with current wl_seat capabilities.
/// @param data Borrowed input context.
/// @param seat Seat proxy advertising capabilities.
/// @param caps Bitwise native capability mask.
static void vgfx_wl_seat_capabilities(void *data, struct wl_proxy *seat, uint32_t caps) {
    vgfx_wayland_input_t *input = data;
    if (!input || !input->connection) return;
    vgfx_wayland_client_api_t *api = &input->connection->api;
    input->capabilities = caps;
    uint32_t version = api->proxy_get_version(seat);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer) {
        input->pointer = api->proxy_marshal_flags(seat, WL_SEAT_GET_POINTER,
            api->pointer_interface, version, 0, NULL);
        if (input->pointer) api->proxy_add_listener(input->pointer,
            (void (**)(void))(void *)&g_pointer_listener, input);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER)) vgfx_wl_release(input, &input->pointer, WL_POINTER_RELEASE);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard) {
        input->keyboard = api->proxy_marshal_flags(seat, WL_SEAT_GET_KEYBOARD,
            api->keyboard_interface, version, 0, NULL);
        if (input->keyboard) api->proxy_add_listener(input->keyboard,
            (void (**)(void))(void *)&g_keyboard_listener, input);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD)) vgfx_wl_release(input, &input->keyboard, WL_KEYBOARD_RELEASE);
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch) {
        input->touch = api->proxy_marshal_flags(
            seat, WL_SEAT_GET_TOUCH, api->touch_interface, version, 0, NULL);
        if (input->touch)
            api->proxy_add_listener(input->touch,
                                    (void (**)(void))(void *)&g_touch_listener,
                                    input);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH)) {
        vgfx_wl_release(input, &input->touch, WL_TOUCH_RELEASE);
        memset(input->contacts, 0, sizeof(input->contacts));
    }
}

/// @brief Ignore the compositor's human-readable seat name.
/// @param data Unused input context.
/// @param seat Unused seat proxy.
/// @param name Unused borrowed name.
static void vgfx_wl_seat_name(void *data, struct wl_proxy *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}
static const vgfx_wl_seat_listener_t g_seat_listener = {vgfx_wl_seat_capabilities, vgfx_wl_seat_name};

/// @copydoc vgfx_wayland_input_open
int vgfx_wayland_input_open(vgfx_wayland_input_t *input, vgfx_wayland_connection_t *connection,
                            struct wl_proxy *surface, struct vgfx_window *window,
                            char *error, uint32_t error_size) {
    if (!input || !connection || !connection->seat || !surface || !window) return 0;
    memset(input, 0, sizeof(*input));
    input->connection = connection;
    input->surface = surface;
    input->window = window;
    input->repeat_rate = 25;
    input->repeat_delay = 600;
    if (connection->api.proxy_add_listener(connection->seat,
            (void (**)(void))(void *)&g_seat_listener, input) != 0 ||
        connection->api.display_roundtrip(connection->display) < 0) {
        if (error && error_size) snprintf(error, error_size, "Wayland seat initialization failed");
        vgfx_wayland_input_close(input);
        return 0;
    }
    return 1;
}

/// @copydoc vgfx_wayland_input_close
void vgfx_wayland_input_close(vgfx_wayland_input_t *input) {
    if (!input) return;
    vgfx_wl_release(input, &input->touch, WL_TOUCH_RELEASE);
    vgfx_wl_release(input, &input->keyboard, WL_KEYBOARD_RELEASE);
    vgfx_wl_release(input, &input->pointer, WL_POINTER_RELEASE);
    vgfx_wl_xkb_close(&input->xkb);
    memset(input, 0, sizeof(*input));
}

/// @copydoc vgfx_wayland_input_tick
int vgfx_wayland_input_tick(vgfx_wayland_input_t *input, int64_t now_ms) {
    if (!input || !input->repeat_active || input->repeat_rate <= 0 ||
        now_ms < input->repeat_at_ms)
        return 0;
    int64_t interval = 1000 / input->repeat_rate;
    if (interval < 1)
        interval = 1;
    int emitted = 0;
    do {
        vgfx_wl_emit_key(input,
                         input->repeat_code,
                         input->repeat_key,
                         1,
                         1,
                         input->repeat_at_ms);
        input->repeat_at_ms += interval;
        emitted++;
    } while (input->repeat_at_ms <= now_ms && emitted < 8);
    if (input->repeat_at_ms <= now_ms)
        input->repeat_at_ms = now_ms + interval;
    return emitted;
}

/// @copydoc vgfx_wayland_input_clamp_timeout
int32_t vgfx_wayland_input_clamp_timeout(const vgfx_wayland_input_t *input,
                                         int32_t requested_ms,
                                         int64_t now_ms) {
    if (!input || !input->repeat_active || input->repeat_rate <= 0)
        return requested_ms;
    int64_t until = input->repeat_at_ms - now_ms;
    if (until < 0)
        until = 0;
    if (until > INT32_MAX)
        until = INT32_MAX;
    if (requested_ms < 0 || until < requested_ms)
        return (int32_t)until;
    return requested_ms;
}
