//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/input/rt_input.c
// Purpose: Keyboard and mouse input state manager for Zanna games. Buffers the
//   platform window's raw key/mouse events between frames and exposes a
//   snapshot API (IsDown, WasPressed, WasReleased, WasClicked) that is stable
//   for the entire duration of a frame update. Callers poll state once per
//   frame after rt_input_begin_frame() and before rt_input_end_frame().
//
// Key invariants:
//   - State is double-buffered: rt_input_begin_frame() captures the current
//     event queue into the "current frame" snapshot. WasPressed/WasReleased
//     compare current and previous snapshots (edge detection). IsDown reflects
//     the current snapshot (level detection).
//   - Key state uses the public ZANNA_KEY_* constants. Windowing backends that
//     emit vgfx key codes must route through rt_keyboard_on_vgfx_key_down/up so
//     ambiguous special-key values are normalized before they enter state.
//   - Mouse button indices use the public ZANNA_MOUSE_BUTTON_* constants:
//     0 = left, 1 = right, 2 = middle, 3/4 = X1/X2. WasClicked is a shorthand
//     for WasPressed && WasReleased
//     in the same frame (single-frame tap detection for quick presses).
//   - Mouse position (X, Y) is in canvas-pixel coordinates (top-left origin,
//     +Y downward), already scaled by the HiDPI scale factor so callers always
//     work in logical canvas pixels.
//   - All state is stored in a GC-managed input context object; there is one
//     context per Canvas window.
//
// Ownership/Lifetime:
//   - Input context objects are GC-managed (rt_obj_new_i64). They are created
//     by rt_graphics.c alongside the Canvas and freed by the GC finalizer.
//
// Links: src/runtime/graphics/input/rt_input.h (public API),
//        src/runtime/graphics/common/rt_graphics.c (Canvas event pump integration),
//        docs/adr/0169-super-modifier-keys-and-studio-viewport-picking.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements frame-coherent keyboard and mouse input state.
 *
 * @details Raw backend events are normalized into stable public key/button
 *          codes, persistent level state, per-frame edge buffers, UTF-8 text,
 *          logical cursor coordinates, wheel deltas, capture/visibility state,
 *          and platform-backed caps-lock and cursor-warp operations.
 */

#include "rt_input.h"
#include "rt_box.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_time.h"

#if RT_PLATFORM_WINDOWS
#include <windows.h>
#elif RT_PLATFORM_MACOS
#include <ApplicationServices/ApplicationServices.h>
#elif RT_PLATFORM_LINUX && defined(ZANNA_ENABLE_GRAPHICS) && !defined(ZANNA_GRAPHICS_HEADLESS) &&  \
    !defined(ZANNA_GRAPHICS_WAYLAND)
#include "vgfx.h"
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#endif

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Key Code Mapping (GLFW <-> vgfx)
//=============================================================================

// vgfx key codes from vgfx.h
#define VGFX_KEY_UNKNOWN 0
#define VGFX_KEY_SPACE ' '
#define VGFX_KEY_0 '0'
#define VGFX_KEY_A 'A'
#define VGFX_KEY_ESCAPE_VG 256
#define VGFX_KEY_ENTER_VG 257
#define VGFX_KEY_LEFT_VG 258
#define VGFX_KEY_RIGHT_VG 259
#define VGFX_KEY_UP_VG 260
#define VGFX_KEY_DOWN_VG 261
#define VGFX_KEY_BACKSPACE_VG 262
#define VGFX_KEY_DELETE_VG 263
#define VGFX_KEY_TAB_VG 264
#define VGFX_KEY_HOME_VG 265
#define VGFX_KEY_END_VG 266
#define VGFX_KEY_PAGE_UP_VG 267
#define VGFX_KEY_PAGE_DOWN_VG 268
#define VGFX_KEY_F1_VG 269
#define VGFX_KEY_F12_VG 280

/// @brief Convert vgfx key code to GLFW-style key code.
/// @param vgfx_key Raw key code produced by the vgfx backend.
/// @return Equivalent public `ZANNA_KEY_*` code, or the original value when no remapping is needed.
static int64_t vgfx_to_glfw(int64_t vgfx_key) {
    // Letters and numbers match directly (ASCII)
    if (vgfx_key >= 'A' && vgfx_key <= 'Z')
        return vgfx_key;
    if (vgfx_key >= '0' && vgfx_key <= '9')
        return vgfx_key;
    if (vgfx_key == VGFX_KEY_SPACE)
        return ZANNA_KEY_SPACE;

    // Map special keys from vgfx to GLFW
    switch (vgfx_key) {
        case VGFX_KEY_ESCAPE_VG:
            return ZANNA_KEY_ESCAPE;
        case VGFX_KEY_ENTER_VG:
            return ZANNA_KEY_ENTER;
        case VGFX_KEY_LEFT_VG:
            return ZANNA_KEY_LEFT;
        case VGFX_KEY_RIGHT_VG:
            return ZANNA_KEY_RIGHT;
        case VGFX_KEY_UP_VG:
            return ZANNA_KEY_UP;
        case VGFX_KEY_DOWN_VG:
            return ZANNA_KEY_DOWN;
        case VGFX_KEY_BACKSPACE_VG:
            return ZANNA_KEY_BACKSPACE;
        case VGFX_KEY_DELETE_VG:
            return ZANNA_KEY_DELETE;
        case VGFX_KEY_TAB_VG:
            return ZANNA_KEY_TAB;
        case VGFX_KEY_HOME_VG:
            return ZANNA_KEY_HOME;
        case VGFX_KEY_END_VG:
            return ZANNA_KEY_END;
        case VGFX_KEY_PAGE_UP_VG:
            return ZANNA_KEY_PAGEUP;
        case VGFX_KEY_PAGE_DOWN_VG:
            return ZANNA_KEY_PAGEDOWN;
        default:
            // Function keys occupy a contiguous range in both vocabularies.
            if (vgfx_key >= VGFX_KEY_F1_VG && vgfx_key <= VGFX_KEY_F12_VG)
                return ZANNA_KEY_F1 + (vgfx_key - VGFX_KEY_F1_VG);
            return vgfx_key;
    }
}

/// @brief Test whether a Unicode scalar is acceptable as typed text.
/// @details Rejects C0/C1 controls, DEL, surrogate code points, private-use ranges, and values
///          beyond Unicode's maximum scalar value.
/// @param ch Candidate Unicode code point.
/// @return true when @p ch may be appended to the per-frame text buffer.
static bool rt_keyboard_codepoint_is_text(int32_t ch) {
    if (ch < 0x20 || ch == 0x7F || ch > 0x10FFFF)
        return false;
    if (ch >= 0x80 && ch <= 0x9F)
        return false;
    if (ch >= 0xD800 && ch <= 0xDFFF)
        return false;
    if ((ch >= 0xE000 && ch <= 0xF8FF) || (ch >= 0xF0000 && ch <= 0xFFFFD) ||
        (ch >= 0x100000 && ch <= 0x10FFFD))
        return false;
    return true;
}

//=============================================================================
// Keyboard State
//=============================================================================

// Current key state (true = pressed)
static bool g_key_state[ZANNA_KEY_MAX];
/// Per-frame edge bitsets mirroring g_pressed_keys / g_released_keys, indexed by key code, so
/// rt_keyboard_was_pressed / _was_released are O(1) instead of a linear scan of the event lists
/// (which matters when a game polls many keys per frame). Cleared whenever the per-frame event
/// counts reset (rt_keyboard_init / rt_keyboard_begin_frame).
static bool g_pressed_this_frame[ZANNA_KEY_MAX];
static bool g_released_this_frame[ZANNA_KEY_MAX];

// Unique keys pressed this frame, in first-edge arrival order.
static int64_t g_pressed_keys[ZANNA_KEY_MAX];
static int g_pressed_count;

// Unique keys released this frame, in first-edge arrival order.
static int64_t g_released_keys[ZANNA_KEY_MAX];
static int g_released_count;

// Text input buffer
static char *g_text_buffer;
static int g_text_length;
static int g_text_capacity;
static bool g_text_input_enabled;

// Caps lock state
static bool g_caps_lock;

// Active canvas for key state queries
static void *g_active_canvas;

// Track if initialized
static bool g_initialized;

/// @brief Clamp a runtime int64 coordinate to the platform cursor-warp int32 range.
/// @param value Runtime coordinate in canvas pixels.
/// @return @p value saturated to the signed 32-bit range accepted by graphics backends.
#if defined(ZANNA_ENABLE_GRAPHICS)
/// @brief Clamp a runtime coordinate to the cursor-warp backend's signed 32-bit range.
/// @param value Runtime coordinate in canvas pixels.
/// @return @p value saturated to [`INT32_MIN`, `INT32_MAX`].
static int32_t rt_input_clamp_i64_to_i32(int64_t value) {
    if (value < (int64_t)INT32_MIN)
        return INT32_MIN;
    if (value > (int64_t)INT32_MAX)
        return INT32_MAX;
    return (int32_t)value;
}
#endif

/// @brief Subtract signed coordinates without invoking overflow.
/// @param value Current coordinate.
/// @param previous Coordinate from the prior frame.
/// @return Exact difference when representable, otherwise the nearest signed 64-bit endpoint.
static int64_t rt_input_saturating_sub_i64(int64_t value, int64_t previous) {
    if (previous > 0 && value < INT64_MIN + previous)
        return INT64_MIN;
    if (previous < 0 && value > INT64_MAX + previous)
        return INT64_MAX;
    return value - previous;
}

/// @brief Round a finite floating-point delta into the signed 64-bit range.
/// @param value Raw delta supplied by an input backend.
/// @return Rounded value, zero for non-finite input, or a saturated endpoint.
static int64_t rt_input_saturating_round_f64_to_i64(double value) {
    if (!isfinite(value))
        return 0;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)llround(value);
}

/// @brief Truncate a finite floating-point accumulator into the signed 64-bit range.
/// @param value Wheel accumulator to convert.
/// @return Truncated value, zero for non-finite input, or a saturated endpoint.
static int64_t rt_input_saturating_trunc_f64_to_i64(double value) {
    if (!isfinite(value))
        return 0;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)value;
}

/// @brief Add one finite wheel delta while keeping the accumulator finite.
/// @param current Existing finite per-frame accumulator.
/// @param delta New backend delta; non-finite values are ignored.
/// @return Exact sum when finite, otherwise the matching finite double endpoint.
static double rt_input_saturating_add_f64(double current, double delta) {
    if (!isfinite(delta))
        return isfinite(current) ? current : 0.0;
    if (!isfinite(current))
        current = 0.0;
    double sum = current + delta;
    if (isfinite(sum))
        return sum;
    return signbit(sum) ? -DBL_MAX : DBL_MAX;
}

/// @brief Ensure the UTF-8 text buffer can append @p needed bytes this frame.
/// @details Text input can arrive in bursts from IME or paste-like platform events. The buffer
/// grows
///          geometrically and retains capacity across frames while `rt_keyboard_begin_frame` resets
///          only the active length.
/// @param needed Minimum active byte capacity required.
/// @return true when the buffer can hold @p needed bytes, false on overflow/allocation failure.
static bool rt_keyboard_reserve_text_bytes(int needed) {
    if (needed < 0)
        return false;
    if (g_text_capacity >= needed)
        return true;
    int new_capacity = g_text_capacity > 0 ? g_text_capacity : 256;
    while (new_capacity < needed) {
        if (new_capacity > INT_MAX / 2)
            return false;
        new_capacity *= 2;
    }
    char *grown = (char *)realloc(g_text_buffer, (size_t)new_capacity);
    if (!grown)
        return false;
    g_text_buffer = grown;
    g_text_capacity = new_capacity;
    return true;
}

// Test hooks let runtime unit tests verify the platform bridge deterministically
// without requiring a real focused window or cursor warp.
/// @brief Override callback for querying caps-lock state.
/// @param canvas Borrowed active canvas, optionally NULL.
/// @return Nonzero when caps lock should be reported as asserted.
typedef int32_t (*rt_caps_lock_query_hook_fn)(void *canvas);
/// @brief Override callback for observing a requested mouse warp.
/// @param canvas Borrowed active mouse canvas.
/// @param x Target X coordinate in canvas pixels.
/// @param y Target Y coordinate in canvas pixels.
typedef void (*rt_mouse_warp_hook_fn)(void *canvas, int64_t x, int64_t y);

static rt_caps_lock_query_hook_fn g_caps_lock_query_hook = NULL;
static rt_mouse_warp_hook_fn g_mouse_warp_hook = NULL;
static void *g_mouse_canvas = NULL;

/// @copydoc rt_input_query_caps_lock_platform()
static int32_t rt_input_query_caps_lock_platform(void);
/// @copydoc rt_input_warp_mouse_platform()
static void rt_input_warp_mouse_platform(int64_t x, int64_t y);

/// @brief Install a caps-lock query test hook.
///
/// Replaces the platform-native caps-lock query (GetKeyState on Windows,
/// CGEventSourceFlagsState on macOS, XkbGetIndicatorState on Linux) with
/// a caller-supplied callback. Used by unit tests to drive caps-lock
/// state deterministically without a live window or OS keyboard.
///
/// Pass NULL to remove the hook and restore native behavior.
///
/// @param hook Test callback invoked with the active canvas pointer; it
///             should return non-zero when caps-lock is asserted. NULL
///             clears the hook.
void rt_input_set_caps_lock_query_hook(rt_caps_lock_query_hook_fn hook) {
    RT_ASSERT_MAIN_THREAD();
    g_caps_lock_query_hook = hook;
}

/// @brief Install a mouse-warp test hook.
///
/// Replaces the platform-native cursor warp (`vgfx_warp_cursor`, which
/// calls `CGWarpMouseCursorPosition` / `SetCursorPos` / `XWarpPointer`)
/// with a caller-supplied callback. Lets unit tests verify that
/// `Mouse.SetPos` issued the expected warp without actually moving the
/// real cursor.
///
/// @param hook Test callback invoked with `(canvas, x, y)`. NULL clears
///             the hook.
void rt_input_set_mouse_warp_hook(rt_mouse_warp_hook_fn hook) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_warp_hook = hook;
}

/// @brief Clear every test hook installed via the setters above.
///
/// Convenience used at the start (and especially the end) of a test case
/// so the next test sees a clean platform-default state. Idempotent:
/// safe to call when no hooks are currently installed.
void rt_input_reset_test_hooks(void) {
    RT_ASSERT_MAIN_THREAD();
    g_caps_lock_query_hook = NULL;
    g_mouse_warp_hook = NULL;
}

/// @brief Resolve the current caps-lock state via the OS or the
///        installed test hook.
///
/// If a `caps_lock_query_hook` has been installed via
/// `rt_input_set_caps_lock_query_hook`, defers to it (passing the active
/// canvas). Otherwise queries the platform:
///   - Windows: `GetKeyState(VK_CAPITAL) & 0x0001`
///   - macOS:   `CGEventSourceFlagsState` and the alpha-shift bit
///   - Linux:   `XkbGetIndicatorState` on the X11 display, opening one
///              transiently if no canvas display is available
///
/// On other platforms or when X11 is not available, falls back to the
/// last toggle observed via key events (`g_caps_lock`).
///
/// @return `1` if caps-lock is currently asserted, `0` otherwise.
static int32_t rt_input_query_caps_lock_platform(void) {
    if (g_caps_lock_query_hook)
        return g_caps_lock_query_hook(g_active_canvas) ? 1 : 0;

#if RT_PLATFORM_WINDOWS
    return (GetKeyState(VK_CAPITAL) & 0x0001) ? 1 : 0;
#elif RT_PLATFORM_MACOS
    CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    return (flags & kCGEventFlagMaskAlphaShift) ? 1 : 0;
#elif RT_PLATFORM_LINUX && defined(ZANNA_ENABLE_GRAPHICS) && !defined(ZANNA_GRAPHICS_HEADLESS) &&  \
    !defined(ZANNA_GRAPHICS_WAYLAND)
    Display *display = NULL;
    int opened_display = 0;
    if (g_active_canvas) {
        vgfx_native_handles_t handles = {0};
        if (vgfx_get_native_handles((vgfx_window_t)g_active_canvas, &handles)) {
            if (handles.backend != VGFX_NATIVE_BACKEND_X11)
                return g_caps_lock ? 1 : 0;
            display = (Display *)handles.display;
        }
    }
#if defined(ZANNA_GRAPHICS_LINUX_AUTO)
    if (!display && getenv("WAYLAND_DISPLAY"))
        return g_caps_lock ? 1 : 0;
#endif
    if (!display) {
        display = XOpenDisplay(NULL);
        opened_display = (display != NULL);
    }
    if (!display)
        return g_caps_lock ? 1 : 0;

    unsigned int indicator_state = 0;
    int status = XkbGetIndicatorState(display, XkbUseCoreKbd, &indicator_state);
    if (opened_display)
        XCloseDisplay(display);
    if (status != Success)
        return g_caps_lock ? 1 : 0;
    return (indicator_state & 0x01U) ? 1 : 0;
#else
    return g_caps_lock ? 1 : 0;
#endif
}

#if defined(ZANNA_ENABLE_GRAPHICS)
#if !(RT_PLATFORM_LINUX && !defined(ZANNA_GRAPHICS_WAYLAND) && !defined(ZANNA_GRAPHICS_HEADLESS))
/// @brief Move the native cursor within a graphics window.
/// @param window Borrowed graphics-window handle.
/// @param x Target backend X coordinate.
/// @param y Target backend Y coordinate.
extern void vgfx_warp_cursor(void *window, int32_t x, int32_t y);
#endif
#endif

/// @brief Move the OS cursor to the given canvas-pixel position.
///
/// Routes through the installed `mouse_warp_hook` (test override) when
/// present; otherwise calls the platform `vgfx_warp_cursor` bridge,
/// which dispatches to `CGWarpMouseCursorPosition` (macOS),
/// `SetCursorPos` (Windows), or `XWarpPointer` (Linux). Coordinates are
/// in the active mouse canvas's pixel space (top-left origin).
///
/// Silent no-op when no canvas is bound or when graphics support is
/// disabled at compile time.
///
/// @param x Target x in canvas pixels.
/// @param y Target y in canvas pixels.
static void rt_input_warp_mouse_platform(int64_t x, int64_t y) {
    if (!g_mouse_canvas)
        return;

    if (g_mouse_warp_hook) {
        g_mouse_warp_hook(g_mouse_canvas, x, y);
        return;
    }

#if defined(ZANNA_ENABLE_GRAPHICS)
    vgfx_warp_cursor(g_mouse_canvas, rt_input_clamp_i64_to_i32(x), rt_input_clamp_i64_to_i32(y));
#else
    (void)x;
    (void)y;
#endif
}

//=============================================================================
// Initialization
//=============================================================================

/// @brief Initialize the keyboard subsystem (zeroes all key state, clears event buffers).
void rt_keyboard_init(void) {
    RT_ASSERT_MAIN_THREAD();
    if (g_initialized)
        return;

    for (int key = 0; key < ZANNA_KEY_MAX; ++key) {
        g_key_state[key] = false;
        g_pressed_this_frame[key] = false;
        g_released_this_frame[key] = false;
    }
    g_pressed_count = 0;
    g_released_count = 0;
    g_text_length = 0;
    g_text_input_enabled = false;
    g_caps_lock = false;
    g_active_canvas = NULL;
    g_initialized = true;
}

/// @brief Clear per-frame pressed/released lists and text buffer. Call once at frame start.
void rt_keyboard_begin_frame(void) {
    RT_ASSERT_MAIN_THREAD();
    // Clear per-frame event lists (and the O(1) edge bitsets that mirror them)
    // cppcheck-suppress incompleteArrayFill -- sizeof names the complete fixed-size array.
    memset(g_pressed_this_frame, 0, sizeof(g_pressed_this_frame));
    // cppcheck-suppress incompleteArrayFill -- sizeof names the complete fixed-size array.
    memset(g_released_this_frame, 0, sizeof(g_released_this_frame));
    g_pressed_count = 0;
    g_released_count = 0;
    g_text_length = 0;
}

/// @brief Apply one normalized key-down transition to level and per-frame edge state.
/// @details Repeat-down input is ignored. A key is appended only for its first
///          down edge in the frame, so the fixed edge array is bounded by the
///          public key-code domain while preserving first-edge arrival order.
/// @param key Public `ZANNA_KEY_*` code.
static void rt_keyboard_record_key_down(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (key <= 0 || key >= ZANNA_KEY_MAX)
        return;

    // Only record press if key wasn't already down
    if (!g_key_state[key]) {
        g_key_state[key] = true;
        if (!g_pressed_this_frame[key]) {
            if (g_pressed_count >= ZANNA_KEY_MAX)
                rt_abort("Keyboard: unique pressed-key invariant violated");
            g_pressed_keys[g_pressed_count++] = key;
            g_pressed_this_frame[key] = true;
        }
    }

    // Caps Lock state is queried from the platform on demand.
}

/// @brief Apply one normalized key-up transition to level and per-frame edge state.
/// @details Up events for keys not currently held are ignored. A key is appended
///          only for its first up edge in the frame, bounding storage while
///          retaining the frame-level "was released" semantics.
/// @param key Public `ZANNA_KEY_*` code.
static void rt_keyboard_record_key_up(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (key <= 0 || key >= ZANNA_KEY_MAX)
        return;

    if (g_key_state[key]) {
        g_key_state[key] = false;
        if (!g_released_this_frame[key]) {
            if (g_released_count >= ZANNA_KEY_MAX)
                rt_abort("Keyboard: unique released-key invariant violated");
            g_released_keys[g_released_count++] = key;
            g_released_this_frame[key] = true;
        }
    }
}

/// @brief Record a key-down event using public ZANNA_KEY_* constants.
/// @param key Public normalized key code.
void rt_keyboard_on_key_down(int64_t key) {
    rt_keyboard_record_key_down(key);
}

/// @brief Record a key-up event using public ZANNA_KEY_* constants.
/// @param key Public normalized key code.
void rt_keyboard_on_key_up(int64_t key) {
    rt_keyboard_record_key_up(key);
}

/// @brief Record a key-down event from a vgfx window event.
/// @param key Raw vgfx key code, normalized before state mutation.
void rt_keyboard_on_vgfx_key_down(int64_t key) {
    rt_keyboard_record_key_down(vgfx_to_glfw(key));
}

/// @brief Record a key-up event from a vgfx window event.
/// @param key Raw vgfx key code, normalized before state mutation.
void rt_keyboard_on_vgfx_key_up(int64_t key) {
    rt_keyboard_record_key_up(vgfx_to_glfw(key));
}

/// @brief Append a text-input character to the per-frame UTF-8 text buffer.
/// @details Input is ignored while text mode is disabled or when @p ch is not an accepted Unicode
///          scalar. Buffer growth failure traps without appending partial UTF-8.
/// @param ch Unicode code point from the platform text-input event.
void rt_keyboard_text_input(int32_t ch) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_text_input_enabled)
        return;

    if (!rt_keyboard_codepoint_is_text(ch))
        return;

    char utf8[4];
    int utf8_len = 0;
    if (ch < 0x80) {
        utf8[0] = (char)ch;
        utf8_len = 1;
    } else if (ch < 0x800) {
        utf8[0] = (char)(0xC0 | (ch >> 6));
        utf8[1] = (char)(0x80 | (ch & 0x3F));
        utf8_len = 2;
    } else if (ch < 0x10000) {
        utf8[0] = (char)(0xE0 | (ch >> 12));
        utf8[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
        utf8[2] = (char)(0x80 | (ch & 0x3F));
        utf8_len = 3;
    } else if (ch <= 0x10FFFF) {
        utf8[0] = (char)(0xF0 | (ch >> 18));
        utf8[1] = (char)(0x80 | ((ch >> 12) & 0x3F));
        utf8[2] = (char)(0x80 | ((ch >> 6) & 0x3F));
        utf8[3] = (char)(0x80 | (ch & 0x3F));
        utf8_len = 4;
    }

    if (utf8_len <= 0 || g_text_length > INT_MAX - utf8_len)
        return;
    if (!rt_keyboard_reserve_text_bytes(g_text_length + utf8_len)) {
        rt_trap("Keyboard: text input buffer allocation failed");
        return;
    }

    memcpy(g_text_buffer + g_text_length, utf8, (size_t)utf8_len);
    g_text_length += utf8_len;
}

/// @brief Bind the keyboard to a canvas window (auto-initializes on first bind).
/// @param canvas Borrowed Canvas handle, or NULL to clear the active binding.
void rt_keyboard_set_canvas(void *canvas) {
    RT_ASSERT_MAIN_THREAD();
    if (canvas)
        rt_keyboard_init();
    g_active_canvas = canvas;
    if (canvas)
        g_caps_lock = rt_input_query_caps_lock_platform() != 0;
}

/// @brief Conditionally release the keyboard's canvas binding.
///
/// Called from the canvas destruction path (`rt_canvas_destroy_window` /
/// `rt_canvas3d_detach_input`) before the underlying vgfx window is torn
/// down. If the keyboard is currently bound to *this* canvas, the
/// binding is dropped so subsequent input queries won't dereference the
/// freed window pointer. If the keyboard is bound to a different canvas
/// (multi-canvas application), the binding is left untouched.
///
/// Safe to call with a NULL canvas (no-op).
///
/// @param canvas Canvas being destroyed. Compared against the active
///               keyboard canvas; only matching bindings are cleared.
void rt_keyboard_clear_canvas_if_matches(void *canvas) {
    RT_ASSERT_MAIN_THREAD();
    if (canvas && g_active_canvas == canvas)
        g_active_canvas = NULL;
}

//=============================================================================
// Polling Methods
//=============================================================================

/// @brief Check whether a key is currently held down (continuous — true every frame while held).
/// @param key Public `ZANNA_KEY_*` code.
/// @return One while a valid key is held, otherwise zero.
int8_t rt_keyboard_is_down(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (key <= 0 || key >= ZANNA_KEY_MAX)
        return 0;

    return g_key_state[key] ? 1 : 0;
}

/// @brief Check whether a key is currently not held down.
/// @param key Public `ZANNA_KEY_*` code.
/// @return One when the key is up; invalid key codes also report up.
int8_t rt_keyboard_is_up(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (key <= 0 || key >= ZANNA_KEY_MAX)
        return 1;

    return g_key_state[key] ? 0 : 1;
}

/// @brief Check whether any key at all is currently held down.
/// @return One when at least one public key is held, otherwise zero.
int8_t rt_keyboard_any_down(void) {
    RT_ASSERT_MAIN_THREAD();
    for (int i = 0; i < ZANNA_KEY_MAX; i++) {
        if (g_key_state[i])
            return 1;
    }
    return 0;
}

/// @brief Get the key code of the first key currently held down (0 if none).
/// @return Lowest held public key code, or zero when no key is down.
int64_t rt_keyboard_get_down(void) {
    RT_ASSERT_MAIN_THREAD();
    for (int i = 0; i < ZANNA_KEY_MAX; i++) {
        if (g_key_state[i])
            return (int64_t)i;
    }
    return 0;
}

//=============================================================================
// Event Methods
//=============================================================================

/// @brief Check whether a key was pressed this frame (edge-triggered — true once on key-down).
/// @param key Public `ZANNA_KEY_*` code.
/// @return One when the key gained its down state this frame, otherwise zero.
int8_t rt_keyboard_was_pressed(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (key <= 0 || key >= ZANNA_KEY_MAX)
        return 0; /* out-of-range keys are never recorded, so they were not pressed */
    return g_pressed_this_frame[key] ? 1 : 0;
}

/// @brief Check whether a key was released this frame (edge-triggered — true once on key-up).
/// @param key Public `ZANNA_KEY_*` code.
/// @return One when the key lost its down state this frame, otherwise zero.
int8_t rt_keyboard_was_released(int64_t key) {
    RT_ASSERT_MAIN_THREAD();
    if (key <= 0 || key >= ZANNA_KEY_MAX)
        return 0; /* out-of-range keys are never recorded, so they were not released */
    return g_released_this_frame[key] ? 1 : 0;
}

/// @brief Get a list of all keys pressed this frame as a sequence of key codes.
/// @return Fresh runtime sequence of boxed public key codes in arrival order.
void *rt_keyboard_get_pressed(void) {
    RT_ASSERT_MAIN_THREAD();
    void *seq = rt_seq_new();
    for (int i = 0; i < g_pressed_count; i++) {
        rt_seq_push(seq, rt_box_i64(g_pressed_keys[i]));
    }
    return seq;
}

/// @brief Get a list of all keys released this frame as a sequence of key codes.
/// @return Fresh runtime sequence of boxed public key codes in arrival order.
void *rt_keyboard_get_released(void) {
    RT_ASSERT_MAIN_THREAD();
    void *seq = rt_seq_new();
    for (int i = 0; i < g_released_count; i++) {
        rt_seq_push(seq, rt_box_i64(g_released_keys[i]));
    }
    return seq;
}

//=============================================================================
// Text Input
//=============================================================================

/// @brief Get the text typed this frame (characters accumulated from text-input events).
/// @return Owned byte-exact UTF-8 runtime string, or an owned empty string when no text arrived.
rt_string rt_keyboard_get_text(void) {
    RT_ASSERT_MAIN_THREAD();
    if (g_text_length == 0)
        return rt_string_from_bytes("", 0);

    return rt_string_from_bytes(g_text_buffer, g_text_length);
}

/// @brief Enable text input mode (characters are collected in the text buffer each frame).
void rt_keyboard_enable_text_input(void) {
    RT_ASSERT_MAIN_THREAD();
    g_text_input_enabled = true;
}

/// @brief Disable text input mode.
void rt_keyboard_disable_text_input(void) {
    RT_ASSERT_MAIN_THREAD();
    g_text_input_enabled = false;
}

//=============================================================================
// Modifier State
//=============================================================================

/// @brief Check whether either Shift key is currently held.
/// @return One when left or right Shift is down, otherwise zero.
int8_t rt_keyboard_shift(void) {
    RT_ASSERT_MAIN_THREAD();
    return (g_key_state[ZANNA_KEY_LSHIFT] || g_key_state[ZANNA_KEY_RSHIFT]) ? 1 : 0;
}

/// @brief Check whether either Ctrl key is currently held.
/// @return One when left or right Control is down, otherwise zero.
int8_t rt_keyboard_ctrl(void) {
    RT_ASSERT_MAIN_THREAD();
    return (g_key_state[ZANNA_KEY_LCTRL] || g_key_state[ZANNA_KEY_RCTRL]) ? 1 : 0;
}

/// @brief Check whether either Alt key is currently held.
/// @return One when left or right Alt is down, otherwise zero.
int8_t rt_keyboard_alt(void) {
    RT_ASSERT_MAIN_THREAD();
    return (g_key_state[ZANNA_KEY_LALT] || g_key_state[ZANNA_KEY_RALT]) ? 1 : 0;
}

/// @brief Check whether Caps Lock is active.
/// @return One when the platform reports Caps Lock active, otherwise zero.
int8_t rt_keyboard_caps_lock(void) {
    RT_ASSERT_MAIN_THREAD();
    g_caps_lock = rt_input_query_caps_lock_platform() != 0;
    return g_caps_lock ? 1 : 0;
}

//=============================================================================
// Key Name Helper
//=============================================================================

/// @brief Get the human-readable name of a key code (e.g., "A", "Space", "F1").
/// @param key Public key code to describe.
/// @return Owned stable display name, using `Unknown` for unmapped values.
rt_string rt_keyboard_key_name(int64_t key) {
    RT_ASSERT_MAIN_THREAD();

    // Letters A-Z and digits 0-9 map directly to their ASCII character. The
    // single byte lives on the stack; rt_string_from_bytes copies exactly the
    // requested length, so no null terminator (or shared static buffer) is needed.
    if (key >= ZANNA_KEY_A && key <= ZANNA_KEY_Z) {
        char letter = (char)key;
        return rt_string_from_bytes(&letter, 1);
    }
    if (key >= ZANNA_KEY_0 && key <= ZANNA_KEY_9) {
        char digit = (char)key;
        return rt_string_from_bytes(&digit, 1);
    }

    // Named keys: a declarative table keeps the keycode->label mapping auditable
    // and diffable. ZANNA_KEY_UNKNOWN and any unlisted code fall through to the
    // "Unknown" default below.
    static const struct {
        int64_t key;
        const char *name;
    } kKeyNames[] = {
        {ZANNA_KEY_SPACE, "Space"},
        {ZANNA_KEY_ESCAPE, "Escape"},
        {ZANNA_KEY_ENTER, "Enter"},
        {ZANNA_KEY_TAB, "Tab"},
        {ZANNA_KEY_BACKSPACE, "Backspace"},
        {ZANNA_KEY_INSERT, "Insert"},
        {ZANNA_KEY_DELETE, "Delete"},
        {ZANNA_KEY_RIGHT, "Right"},
        {ZANNA_KEY_LEFT, "Left"},
        {ZANNA_KEY_DOWN, "Down"},
        {ZANNA_KEY_UP, "Up"},
        {ZANNA_KEY_PAGEUP, "PageUp"},
        {ZANNA_KEY_PAGEDOWN, "PageDown"},
        {ZANNA_KEY_HOME, "Home"},
        {ZANNA_KEY_END, "End"},
        {ZANNA_KEY_F1, "F1"},
        {ZANNA_KEY_F2, "F2"},
        {ZANNA_KEY_F3, "F3"},
        {ZANNA_KEY_F4, "F4"},
        {ZANNA_KEY_F5, "F5"},
        {ZANNA_KEY_F6, "F6"},
        {ZANNA_KEY_F7, "F7"},
        {ZANNA_KEY_F8, "F8"},
        {ZANNA_KEY_F9, "F9"},
        {ZANNA_KEY_F10, "F10"},
        {ZANNA_KEY_F11, "F11"},
        {ZANNA_KEY_F12, "F12"},
        {ZANNA_KEY_LSHIFT, "Left Shift"},
        {ZANNA_KEY_RSHIFT, "Right Shift"},
        {ZANNA_KEY_LCTRL, "Left Ctrl"},
        {ZANNA_KEY_RCTRL, "Right Ctrl"},
        {ZANNA_KEY_LALT, "Left Alt"},
        {ZANNA_KEY_RALT, "Right Alt"},
        {ZANNA_KEY_LSUPER, "Left Super"},
        {ZANNA_KEY_RSUPER, "Right Super"},
        {ZANNA_KEY_MINUS, "Minus"},
        {ZANNA_KEY_EQUALS, "Equals"},
        {ZANNA_KEY_LBRACKET, "Left Bracket"},
        {ZANNA_KEY_RBRACKET, "Right Bracket"},
        {ZANNA_KEY_BACKSLASH, "Backslash"},
        {ZANNA_KEY_SEMICOLON, "Semicolon"},
        {ZANNA_KEY_QUOTE, "Quote"},
        {ZANNA_KEY_GRAVE, "Grave"},
        {ZANNA_KEY_COMMA, "Comma"},
        {ZANNA_KEY_PERIOD, "Period"},
        {ZANNA_KEY_SLASH, "Slash"},
        {ZANNA_KEY_NUM0, "Numpad 0"},
        {ZANNA_KEY_NUM1, "Numpad 1"},
        {ZANNA_KEY_NUM2, "Numpad 2"},
        {ZANNA_KEY_NUM3, "Numpad 3"},
        {ZANNA_KEY_NUM4, "Numpad 4"},
        {ZANNA_KEY_NUM5, "Numpad 5"},
        {ZANNA_KEY_NUM6, "Numpad 6"},
        {ZANNA_KEY_NUM7, "Numpad 7"},
        {ZANNA_KEY_NUM8, "Numpad 8"},
        {ZANNA_KEY_NUM9, "Numpad 9"},
        {ZANNA_KEY_NUMADD, "Numpad Add"},
        {ZANNA_KEY_NUMSUB, "Numpad Subtract"},
        {ZANNA_KEY_NUMMUL, "Numpad Multiply"},
        {ZANNA_KEY_NUMDIV, "Numpad Divide"},
        {ZANNA_KEY_NUMENTER, "Numpad Enter"},
        {ZANNA_KEY_NUMDOT, "Numpad Decimal"},
    };

    const char *name = "Unknown";
    for (size_t i = 0; i < sizeof(kKeyNames) / sizeof(kKeyNames[0]); ++i) {
        if (kKeyNames[i].key == key) {
            name = kKeyNames[i].name;
            break;
        }
    }

    return rt_string_from_bytes(name, strlen(name));
}

//=============================================================================
// Key Code Constant Getters
//
// Each accessor below returns one of the platform-independent ZANNA_KEY_*
// integer constants defined in rt_input.h. These wrappers exist so Zia
// and BASIC programs can refer to keys by name (e.g. `Keyboard.IsDown(
// Keyboard.Key.A)`) rather than hard-coding magic integers, which would
// drift if the key-code numbering ever changed.
//
// All getters are pure (no side effects), thread-safe (read-only access
// to compile-time constants), and constant-folded by the compiler in
// almost every call site, so the wrapper overhead is zero in practice.
// Each function takes no arguments and returns the canonical i64 code.
//=============================================================================

/// @brief Key-code constant for the unknown / unmapped key sentinel.
/// @return `ZANNA_KEY_UNKNOWN`.
int64_t rt_keyboard_key_unknown(void) {
    return ZANNA_KEY_UNKNOWN;
}

/// @brief Key-code constant for the A key.
/// @return `ZANNA_KEY_A`.
int64_t rt_keyboard_key_a(void) {
    return ZANNA_KEY_A;
}

/// @brief Key-code constant for the B key.
/// @return `ZANNA_KEY_B`.
int64_t rt_keyboard_key_b(void) {
    return ZANNA_KEY_B;
}

/// @brief Key-code constant for the C key.
/// @return `ZANNA_KEY_C`.
int64_t rt_keyboard_key_c(void) {
    return ZANNA_KEY_C;
}

/// @brief Key-code constant for the D key.
/// @return `ZANNA_KEY_D`.
int64_t rt_keyboard_key_d(void) {
    return ZANNA_KEY_D;
}

/// @brief Key-code constant for the E key.
/// @return `ZANNA_KEY_E`.
int64_t rt_keyboard_key_e(void) {
    return ZANNA_KEY_E;
}

/// @brief Key-code constant for the F key.
/// @return `ZANNA_KEY_F`.
int64_t rt_keyboard_key_f(void) {
    return ZANNA_KEY_F;
}

/// @brief Key-code constant for the G key.
/// @return `ZANNA_KEY_G`.
int64_t rt_keyboard_key_g(void) {
    return ZANNA_KEY_G;
}

/// @brief Key-code constant for the H key.
/// @return `ZANNA_KEY_H`.
int64_t rt_keyboard_key_h(void) {
    return ZANNA_KEY_H;
}

/// @brief Key-code constant for the I key.
/// @return `ZANNA_KEY_I`.
int64_t rt_keyboard_key_i(void) {
    return ZANNA_KEY_I;
}

/// @brief Key-code constant for the J key.
/// @return `ZANNA_KEY_J`.
int64_t rt_keyboard_key_j(void) {
    return ZANNA_KEY_J;
}

/// @brief Key-code constant for the K key.
/// @return `ZANNA_KEY_K`.
int64_t rt_keyboard_key_k(void) {
    return ZANNA_KEY_K;
}

/// @brief Key-code constant for the L key.
/// @return `ZANNA_KEY_L`.
int64_t rt_keyboard_key_l(void) {
    return ZANNA_KEY_L;
}

/// @brief Key-code constant for the M key.
/// @return `ZANNA_KEY_M`.
int64_t rt_keyboard_key_m(void) {
    return ZANNA_KEY_M;
}

/// @brief Key-code constant for the N key.
/// @return `ZANNA_KEY_N`.
int64_t rt_keyboard_key_n(void) {
    return ZANNA_KEY_N;
}

/// @brief Key-code constant for the O key.
/// @return `ZANNA_KEY_O`.
int64_t rt_keyboard_key_o(void) {
    return ZANNA_KEY_O;
}

/// @brief Key-code constant for the P key.
/// @return `ZANNA_KEY_P`.
int64_t rt_keyboard_key_p(void) {
    return ZANNA_KEY_P;
}

/// @brief Key-code constant for the Q key.
/// @return `ZANNA_KEY_Q`.
int64_t rt_keyboard_key_q(void) {
    return ZANNA_KEY_Q;
}

/// @brief Key-code constant for the R key.
/// @return `ZANNA_KEY_R`.
int64_t rt_keyboard_key_r(void) {
    return ZANNA_KEY_R;
}

/// @brief Key-code constant for the S key.
/// @return `ZANNA_KEY_S`.
int64_t rt_keyboard_key_s(void) {
    return ZANNA_KEY_S;
}

/// @brief Key-code constant for the T key.
/// @return `ZANNA_KEY_T`.
int64_t rt_keyboard_key_t(void) {
    return ZANNA_KEY_T;
}

/// @brief Key-code constant for the U key.
/// @return `ZANNA_KEY_U`.
int64_t rt_keyboard_key_u(void) {
    return ZANNA_KEY_U;
}

/// @brief Key-code constant for the V key.
/// @return `ZANNA_KEY_V`.
int64_t rt_keyboard_key_v(void) {
    return ZANNA_KEY_V;
}

/// @brief Key-code constant for the W key.
/// @return `ZANNA_KEY_W`.
int64_t rt_keyboard_key_w(void) {
    return ZANNA_KEY_W;
}

/// @brief Key-code constant for the X key.
/// @return `ZANNA_KEY_X`.
int64_t rt_keyboard_key_x(void) {
    return ZANNA_KEY_X;
}

/// @brief Key-code constant for the Y key.
/// @return `ZANNA_KEY_Y`.
int64_t rt_keyboard_key_y(void) {
    return ZANNA_KEY_Y;
}

/// @brief Key-code constant for the Z key.
/// @return `ZANNA_KEY_Z`.
int64_t rt_keyboard_key_z(void) {
    return ZANNA_KEY_Z;
}

/// @brief Key-code constant for the 0 (zero) row-digit key.
/// @return `ZANNA_KEY_0`.
int64_t rt_keyboard_key_0(void) {
    return ZANNA_KEY_0;
}

/// @brief Key-code constant for the 1 row-digit key.
/// @return `ZANNA_KEY_1`.
int64_t rt_keyboard_key_1(void) {
    return ZANNA_KEY_1;
}

/// @brief Key-code constant for the 2 row-digit key.
/// @return `ZANNA_KEY_2`.
int64_t rt_keyboard_key_2(void) {
    return ZANNA_KEY_2;
}

/// @brief Key-code constant for the 3 row-digit key.
/// @return `ZANNA_KEY_3`.
int64_t rt_keyboard_key_3(void) {
    return ZANNA_KEY_3;
}

/// @brief Key-code constant for the 4 row-digit key.
/// @return `ZANNA_KEY_4`.
int64_t rt_keyboard_key_4(void) {
    return ZANNA_KEY_4;
}

/// @brief Key-code constant for the 5 row-digit key.
/// @return `ZANNA_KEY_5`.
int64_t rt_keyboard_key_5(void) {
    return ZANNA_KEY_5;
}

/// @brief Key-code constant for the 6 row-digit key.
/// @return `ZANNA_KEY_6`.
int64_t rt_keyboard_key_6(void) {
    return ZANNA_KEY_6;
}

/// @brief Key-code constant for the 7 row-digit key.
/// @return `ZANNA_KEY_7`.
int64_t rt_keyboard_key_7(void) {
    return ZANNA_KEY_7;
}

/// @brief Key-code constant for the 8 row-digit key.
/// @return `ZANNA_KEY_8`.
int64_t rt_keyboard_key_8(void) {
    return ZANNA_KEY_8;
}

/// @brief Key-code constant for the 9 row-digit key.
/// @return `ZANNA_KEY_9`.
int64_t rt_keyboard_key_9(void) {
    return ZANNA_KEY_9;
}

/// @brief Key-code constant for the F1 function key.
/// @return `ZANNA_KEY_F1`.
int64_t rt_keyboard_key_f1(void) {
    return ZANNA_KEY_F1;
}

/// @brief Key-code constant for the F2 function key.
/// @return `ZANNA_KEY_F2`.
int64_t rt_keyboard_key_f2(void) {
    return ZANNA_KEY_F2;
}

/// @brief Key-code constant for the F3 function key.
/// @return `ZANNA_KEY_F3`.
int64_t rt_keyboard_key_f3(void) {
    return ZANNA_KEY_F3;
}

/// @brief Key-code constant for the F4 function key.
/// @return `ZANNA_KEY_F4`.
int64_t rt_keyboard_key_f4(void) {
    return ZANNA_KEY_F4;
}

/// @brief Key-code constant for the F5 function key.
/// @return `ZANNA_KEY_F5`.
int64_t rt_keyboard_key_f5(void) {
    return ZANNA_KEY_F5;
}

/// @brief Key-code constant for the F6 function key.
/// @return `ZANNA_KEY_F6`.
int64_t rt_keyboard_key_f6(void) {
    return ZANNA_KEY_F6;
}

/// @brief Key-code constant for the F7 function key.
/// @return `ZANNA_KEY_F7`.
int64_t rt_keyboard_key_f7(void) {
    return ZANNA_KEY_F7;
}

/// @brief Key-code constant for the F8 function key.
/// @return `ZANNA_KEY_F8`.
int64_t rt_keyboard_key_f8(void) {
    return ZANNA_KEY_F8;
}

/// @brief Key-code constant for the F9 function key.
/// @return `ZANNA_KEY_F9`.
int64_t rt_keyboard_key_f9(void) {
    return ZANNA_KEY_F9;
}

/// @brief Key-code constant for the F10 function key.
/// @return `ZANNA_KEY_F10`.
int64_t rt_keyboard_key_f10(void) {
    return ZANNA_KEY_F10;
}

/// @brief Key-code constant for the F11 function key.
/// @return `ZANNA_KEY_F11`.
int64_t rt_keyboard_key_f11(void) {
    return ZANNA_KEY_F11;
}

/// @brief Key-code constant for the F12 function key.
/// @return `ZANNA_KEY_F12`.
int64_t rt_keyboard_key_f12(void) {
    return ZANNA_KEY_F12;
}

/// @brief Key-code constant for the Up arrow key.
/// @return `ZANNA_KEY_UP`.
int64_t rt_keyboard_key_up(void) {
    return ZANNA_KEY_UP;
}

/// @brief Key-code constant for the Down arrow key.
/// @return `ZANNA_KEY_DOWN`.
int64_t rt_keyboard_key_down(void) {
    return ZANNA_KEY_DOWN;
}

/// @brief Key-code constant for the Left arrow key.
/// @return `ZANNA_KEY_LEFT`.
int64_t rt_keyboard_key_left(void) {
    return ZANNA_KEY_LEFT;
}

/// @brief Key-code constant for the Right arrow key.
/// @return `ZANNA_KEY_RIGHT`.
int64_t rt_keyboard_key_right(void) {
    return ZANNA_KEY_RIGHT;
}

/// @brief Key-code constant for the Home navigation key.
/// @return `ZANNA_KEY_HOME`.
int64_t rt_keyboard_key_home(void) {
    return ZANNA_KEY_HOME;
}

/// @brief Key-code constant for the End navigation key.
/// @return `ZANNA_KEY_END`.
int64_t rt_keyboard_key_end(void) {
    return ZANNA_KEY_END;
}

/// @brief Key-code constant for the Page Up navigation key.
/// @return `ZANNA_KEY_PAGEUP`.
int64_t rt_keyboard_key_pageup(void) {
    return ZANNA_KEY_PAGEUP;
}

/// @brief Key-code constant for the Page Down navigation key.
/// @return `ZANNA_KEY_PAGEDOWN`.
int64_t rt_keyboard_key_pagedown(void) {
    return ZANNA_KEY_PAGEDOWN;
}

/// @brief Key-code constant for the Insert editing key.
/// @return `ZANNA_KEY_INSERT`.
int64_t rt_keyboard_key_insert(void) {
    return ZANNA_KEY_INSERT;
}

/// @brief Key-code constant for the Delete editing key (forward delete).
/// @return `ZANNA_KEY_DELETE`.
int64_t rt_keyboard_key_delete(void) {
    return ZANNA_KEY_DELETE;
}

/// @brief Key-code constant for the Backspace editing key (backward delete).
/// @return `ZANNA_KEY_BACKSPACE`.
int64_t rt_keyboard_key_backspace(void) {
    return ZANNA_KEY_BACKSPACE;
}

/// @brief Key-code constant for the Tab key.
/// @return `ZANNA_KEY_TAB`.
int64_t rt_keyboard_key_tab(void) {
    return ZANNA_KEY_TAB;
}

/// @brief Key-code constant for the main Enter / Return key (numpad enter
///        is `Key.NumEnter`).
/// @return `ZANNA_KEY_ENTER`.
int64_t rt_keyboard_key_enter(void) {
    return ZANNA_KEY_ENTER;
}

/// @brief Key-code constant for the Space bar.
/// @return `ZANNA_KEY_SPACE`.
int64_t rt_keyboard_key_space(void) {
    return ZANNA_KEY_SPACE;
}

/// @brief Key-code constant for the Escape key.
/// @return `ZANNA_KEY_ESCAPE`.
int64_t rt_keyboard_key_escape(void) {
    return ZANNA_KEY_ESCAPE;
}

/// @brief Key-code constant for the Left Shift modifier specifically.
/// @return `ZANNA_KEY_LSHIFT`.
int64_t rt_keyboard_key_lshift(void) {
    return ZANNA_KEY_LSHIFT;
}

/// @brief Key-code constant for the Right Shift modifier specifically.
/// @return `ZANNA_KEY_RSHIFT`.
int64_t rt_keyboard_key_rshift(void) {
    return ZANNA_KEY_RSHIFT;
}

/// @brief Key-code constant for the Left Ctrl modifier specifically.
/// @return `ZANNA_KEY_LCTRL`.
int64_t rt_keyboard_key_lctrl(void) {
    return ZANNA_KEY_LCTRL;
}

/// @brief Key-code constant for the Right Ctrl modifier specifically.
/// @return `ZANNA_KEY_RCTRL`.
int64_t rt_keyboard_key_rctrl(void) {
    return ZANNA_KEY_RCTRL;
}

/// @brief Key-code constant for the Left Alt modifier specifically.
/// @return `ZANNA_KEY_LALT`.
int64_t rt_keyboard_key_lalt(void) {
    return ZANNA_KEY_LALT;
}

/// @brief Key-code constant for the Right Alt modifier specifically (AltGr
///        on European layouts).
/// @return `ZANNA_KEY_RALT`.
int64_t rt_keyboard_key_ralt(void) {
    return ZANNA_KEY_RALT;
}

/// @brief Key-code constant for the Left Super modifier (Command on macOS,
///        Windows key on Windows).
/// @return `ZANNA_KEY_LSUPER`.
int64_t rt_keyboard_key_lsuper(void) {
    return ZANNA_KEY_LSUPER;
}

/// @brief Key-code constant for the Right Super modifier (Command on macOS,
///        Windows key on Windows).
/// @return `ZANNA_KEY_RSUPER`.
int64_t rt_keyboard_key_rsuper(void) {
    return ZANNA_KEY_RSUPER;
}

/// @brief Key-code constant for the Minus / Hyphen punctuation key.
/// @return `ZANNA_KEY_MINUS`.
int64_t rt_keyboard_key_minus(void) {
    return ZANNA_KEY_MINUS;
}

/// @brief Key-code constant for the Equals / Plus punctuation key.
/// @return `ZANNA_KEY_EQUALS`.
int64_t rt_keyboard_key_equals(void) {
    return ZANNA_KEY_EQUALS;
}

/// @brief Key-code constant for the Left Bracket `[` punctuation key.
/// @return `ZANNA_KEY_LBRACKET`.
int64_t rt_keyboard_key_lbracket(void) {
    return ZANNA_KEY_LBRACKET;
}

/// @brief Key-code constant for the Right Bracket `]` punctuation key.
/// @return `ZANNA_KEY_RBRACKET`.
int64_t rt_keyboard_key_rbracket(void) {
    return ZANNA_KEY_RBRACKET;
}

/// @brief Key-code constant for the Backslash `\\` punctuation key.
/// @return `ZANNA_KEY_BACKSLASH`.
int64_t rt_keyboard_key_backslash(void) {
    return ZANNA_KEY_BACKSLASH;
}

/// @brief Key-code constant for the Semicolon `;` punctuation key.
/// @return `ZANNA_KEY_SEMICOLON`.
int64_t rt_keyboard_key_semicolon(void) {
    return ZANNA_KEY_SEMICOLON;
}

/// @brief Key-code constant for the Quote / Apostrophe `'` punctuation key.
/// @return `ZANNA_KEY_QUOTE`.
int64_t rt_keyboard_key_quote(void) {
    return ZANNA_KEY_QUOTE;
}

/// @brief Key-code constant for the Grave / Backtick `` ` `` punctuation key
///        (typically below Esc on US keyboards).
/// @return `ZANNA_KEY_GRAVE`.
int64_t rt_keyboard_key_grave(void) {
    return ZANNA_KEY_GRAVE;
}

/// @brief Key-code constant for the Comma `,` punctuation key.
/// @return `ZANNA_KEY_COMMA`.
int64_t rt_keyboard_key_comma(void) {
    return ZANNA_KEY_COMMA;
}

/// @brief Key-code constant for the Period `.` punctuation key.
/// @return `ZANNA_KEY_PERIOD`.
int64_t rt_keyboard_key_period(void) {
    return ZANNA_KEY_PERIOD;
}

/// @brief Key-code constant for the Slash `/` punctuation key.
/// @return `ZANNA_KEY_SLASH`.
int64_t rt_keyboard_key_slash(void) {
    return ZANNA_KEY_SLASH;
}

/// @brief Key-code constant for the numpad 0 key.
/// @return `ZANNA_KEY_NUM0`.
int64_t rt_keyboard_key_num0(void) {
    return ZANNA_KEY_NUM0;
}

/// @brief Key-code constant for the numpad 1 key.
/// @return `ZANNA_KEY_NUM1`.
int64_t rt_keyboard_key_num1(void) {
    return ZANNA_KEY_NUM1;
}

/// @brief Key-code constant for the numpad 2 key.
/// @return `ZANNA_KEY_NUM2`.
int64_t rt_keyboard_key_num2(void) {
    return ZANNA_KEY_NUM2;
}

/// @brief Key-code constant for the numpad 3 key.
/// @return `ZANNA_KEY_NUM3`.
int64_t rt_keyboard_key_num3(void) {
    return ZANNA_KEY_NUM3;
}

/// @brief Key-code constant for the numpad 4 key.
/// @return `ZANNA_KEY_NUM4`.
int64_t rt_keyboard_key_num4(void) {
    return ZANNA_KEY_NUM4;
}

/// @brief Key-code constant for the numpad 5 key.
/// @return `ZANNA_KEY_NUM5`.
int64_t rt_keyboard_key_num5(void) {
    return ZANNA_KEY_NUM5;
}

/// @brief Key-code constant for the numpad 6 key.
/// @return `ZANNA_KEY_NUM6`.
int64_t rt_keyboard_key_num6(void) {
    return ZANNA_KEY_NUM6;
}

/// @brief Key-code constant for the numpad 7 key.
/// @return `ZANNA_KEY_NUM7`.
int64_t rt_keyboard_key_num7(void) {
    return ZANNA_KEY_NUM7;
}

/// @brief Key-code constant for the numpad 8 key.
/// @return `ZANNA_KEY_NUM8`.
int64_t rt_keyboard_key_num8(void) {
    return ZANNA_KEY_NUM8;
}

/// @brief Key-code constant for the numpad 9 key.
/// @return `ZANNA_KEY_NUM9`.
int64_t rt_keyboard_key_num9(void) {
    return ZANNA_KEY_NUM9;
}

/// @brief Key-code constant for the numpad Add `+` key.
/// @return `ZANNA_KEY_NUMADD`.
int64_t rt_keyboard_key_numadd(void) {
    return ZANNA_KEY_NUMADD;
}

/// @brief Key-code constant for the numpad Subtract `-` key.
/// @return `ZANNA_KEY_NUMSUB`.
int64_t rt_keyboard_key_numsub(void) {
    return ZANNA_KEY_NUMSUB;
}

/// @brief Key-code constant for the numpad Multiply `*` key.
/// @return `ZANNA_KEY_NUMMUL`.
int64_t rt_keyboard_key_nummul(void) {
    return ZANNA_KEY_NUMMUL;
}

/// @brief Key-code constant for the numpad Divide `/` key.
/// @return `ZANNA_KEY_NUMDIV`.
int64_t rt_keyboard_key_numdiv(void) {
    return ZANNA_KEY_NUMDIV;
}

/// @brief Key-code constant for the numpad Enter key (distinct from the
///        main Enter key).
/// @return `ZANNA_KEY_NUMENTER`.
int64_t rt_keyboard_key_numenter(void) {
    return ZANNA_KEY_NUMENTER;
}

/// @brief Key-code constant for the numpad Decimal `.` key.
/// @return `ZANNA_KEY_NUMDOT`.
int64_t rt_keyboard_key_numdot(void) {
    return ZANNA_KEY_NUMDOT;
}

//=============================================================================
// Mouse Input Implementation
//=============================================================================

// Mouse state
static int64_t g_mouse_x = 0;
static int64_t g_mouse_y = 0;
static int64_t g_mouse_prev_x = 0;
static int64_t g_mouse_prev_y = 0;
static int64_t g_mouse_delta_x = 0;
static int64_t g_mouse_delta_y = 0;
static double g_mouse_wheel_x = 0.0;
static double g_mouse_wheel_y = 0.0;

// Button state arrays
static bool g_mouse_button_state[ZANNA_MOUSE_BUTTON_MAX];
static bool g_mouse_button_pressed[ZANNA_MOUSE_BUTTON_MAX];
static bool g_mouse_button_released[ZANNA_MOUSE_BUTTON_MAX];

// Click detection - track press times for each button
static int64_t g_mouse_press_time[ZANNA_MOUSE_BUTTON_MAX];
static int64_t g_mouse_last_click_time[ZANNA_MOUSE_BUTTON_MAX];
static bool g_mouse_clicked[ZANNA_MOUSE_BUTTON_MAX];
static bool g_mouse_double_clicked[ZANNA_MOUSE_BUTTON_MAX];

// Click detection constants (in milliseconds)
#define CLICK_MAX_DURATION_MS 300
#define DOUBLE_CLICK_MAX_INTERVAL_MS 400

// Cursor state
static bool g_mouse_hidden = false;
static bool g_mouse_captured = false;

// Relative (raw) mouse mode state. `requested` is the application's ask;
// `native` records whether the platform window actually delivers raw deltas
// (reported back by the Canvas3D poll). The f64 deltas carry sub-pixel
// precision alongside the rounded i64 g_mouse_delta_x/y.
static bool g_mouse_relative_requested = false;
static bool g_mouse_relative_native = false;
static double g_mouse_delta_fx = 0.0;
static double g_mouse_delta_fy = 0.0;

// Initialization flag
static bool g_mouse_initialized = false;

/// @brief Read the runtime monotonic clock in milliseconds.
///
/// Internal helper used by the mouse-button bookkeeping below to time
/// click vs. hold intervals (`CLICK_MAX_DURATION_MS`,
/// `DOUBLE_CLICK_MAX_INTERVAL_MS`). Routes through the shared
/// `rt_clock_ticks_us` source so it stays consistent with other timing
/// surfaces (`Time.NowMs`, `Stopwatch`).
///
/// @return Current monotonic time in milliseconds since process start.
static int64_t get_time_ms(void) {
    return rt_clock_ticks_us() / 1000;
}

/// @brief Initialize the mouse subsystem to a clean state.
///
/// Zeros the pointer position, deltas, wheel accumulators, and every
/// per-button state array. Called automatically the first time a Canvas
/// binds the mouse via `rt_mouse_set_canvas` and is idempotent — repeat
/// calls are safe and short-circuit on the `g_mouse_initialized` flag.
void rt_mouse_init(void) {
    RT_ASSERT_MAIN_THREAD();
    if (g_mouse_initialized)
        return;

    g_mouse_x = 0;
    g_mouse_y = 0;
    g_mouse_prev_x = 0;
    g_mouse_prev_y = 0;
    g_mouse_delta_x = 0;
    g_mouse_delta_y = 0;
    g_mouse_wheel_x = 0.0;
    g_mouse_wheel_y = 0.0;
    g_mouse_hidden = false;
    g_mouse_captured = false;
    g_mouse_relative_requested = false;
    g_mouse_relative_native = false;
    g_mouse_delta_fx = 0.0;
    g_mouse_delta_fy = 0.0;
    g_mouse_canvas = NULL;

    for (int i = 0; i < ZANNA_MOUSE_BUTTON_MAX; i++) {
        g_mouse_button_state[i] = false;
        g_mouse_button_pressed[i] = false;
        g_mouse_button_released[i] = false;
        g_mouse_press_time[i] = 0;
        g_mouse_last_click_time[i] = -1;
        g_mouse_clicked[i] = false;
        g_mouse_double_clicked[i] = false;
    }

    g_mouse_initialized = true;
}

/// @brief Snapshot mouse state for the new frame.
///
/// Computes per-frame position deltas (`delta_x`, `delta_y`) by
/// subtracting the previous frame's pointer position, advances the
/// `prev_x/y` reference, and resets the per-frame event arrays
/// (`pressed`, `released`, `clicked`, `double_clicked`) and wheel
/// accumulators back to zero. Called once per game frame between event
/// pumping and game-loop user code.
void rt_mouse_begin_frame(void) {
    RT_ASSERT_MAIN_THREAD();
    // Calculate delta from previous position. Poll paths that pump events after
    // this call refresh the delta via rt_mouse_finalize_frame() so it describes
    // this frame's motion instead of lagging one poll behind Mouse.X/Y.
    g_mouse_delta_x = rt_input_saturating_sub_i64(g_mouse_x, g_mouse_prev_x);
    g_mouse_delta_y = rt_input_saturating_sub_i64(g_mouse_y, g_mouse_prev_y);
    g_mouse_prev_x = g_mouse_x;
    g_mouse_prev_y = g_mouse_y;
    g_mouse_delta_fx = (double)g_mouse_delta_x;
    g_mouse_delta_fy = (double)g_mouse_delta_y;

    // Reset per-frame event arrays
    for (int i = 0; i < ZANNA_MOUSE_BUTTON_MAX; i++) {
        g_mouse_button_pressed[i] = false;
        g_mouse_button_released[i] = false;
        g_mouse_clicked[i] = false;
        g_mouse_double_clicked[i] = false;
    }

    // Reset wheel deltas
    g_mouse_wheel_x = 0.0;
    g_mouse_wheel_y = 0.0;
}

/// @brief Recompute the absolute mouse delta after this frame's events are pumped.
///
/// `rt_mouse_begin_frame` runs before the poll drains the OS event queue, so the
/// delta it computes describes the *previous* frame's motion. Poll paths call this
/// after event processing so `Mouse.DeltaX/Y` and `Mouse.X/Y` agree on the same
/// frame. Relative-mode overrides (`rt_mouse_force_delta*`) run afterwards in the
/// Canvas3D poll and take precedence.
void rt_mouse_finalize_frame(void) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_delta_x = rt_input_saturating_sub_i64(g_mouse_x, g_mouse_prev_x);
    g_mouse_delta_y = rt_input_saturating_sub_i64(g_mouse_y, g_mouse_prev_y);
    g_mouse_prev_x = g_mouse_x;
    g_mouse_prev_y = g_mouse_y;
    g_mouse_delta_fx = (double)g_mouse_delta_x;
    g_mouse_delta_fy = (double)g_mouse_delta_y;
}

/// @brief Forward an OS mouse-move event into the runtime state.
///
/// Called by `rt_canvas_poll` for every `VGFX_EVENT_MOUSE_MOVE` event,
/// after applying coordinate-scale conversion. Coordinates are in
/// canvas-pixel space (top-left origin, +Y down), already scaled by
/// the HiDPI factor so callers see logical pixels.
///
/// @param x New pointer x in canvas pixels.
/// @param y New pointer y in canvas pixels.
void rt_mouse_update_pos(int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_x = x;
    g_mouse_y = y;
}

/// @brief Override the mouse delta values for the current frame.
///
/// Used by tests and synthetic input paths (e.g. replay) that don't go
/// through `update_pos` but still want `Mouse.DeltaX`/`DeltaY` to read
/// expected values. Bypasses the normal `prev_x/y`-based delta computation;
/// the next `begin_frame` call will compute deltas normally again.
///
/// @param dx Forced delta x in canvas pixels.
/// @param dy Forced delta y in canvas pixels.
void rt_mouse_force_delta(int64_t dx, int64_t dy) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_delta_x = dx;
    g_mouse_delta_y = dy;
    g_mouse_delta_fx = (double)dx;
    g_mouse_delta_fy = (double)dy;
}

/// @brief Override the mouse delta with sub-pixel precision.
///
/// Relative-mode variant of `rt_mouse_force_delta`: stores the exact f64
/// motion for `rt_mouse_delta_xf/yf` and a round-to-nearest i64 for the
/// legacy `Mouse.DeltaX/DeltaY` surface. Called by the Canvas3D poll while
/// native raw deltas are active.
///
/// @param dx Sub-pixel horizontal motion for this frame.
/// @param dy Sub-pixel vertical motion for this frame (positive = down).
void rt_mouse_force_delta_f(double dx, double dy) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_delta_fx = isfinite(dx) ? dx : 0.0;
    g_mouse_delta_fy = isfinite(dy) ? dy : 0.0;
    g_mouse_delta_x = rt_input_saturating_round_f64_to_i64(dx);
    g_mouse_delta_y = rt_input_saturating_round_f64_to_i64(dy);
}

/// @brief Forward an OS mouse-button-press event into the runtime state.
///
/// Called by `rt_canvas_poll` for every `VGFX_EVENT_MOUSE_DOWN`. Only
/// the *transition* from up to down sets `pressed[button]` to true and
/// records the press timestamp; repeat-down events for an already-held
/// button are filtered out (BIOS auto-repeat protection).
///
/// Out-of-range button indices are silently ignored.
///
/// @param button Mouse button index (`0`=left, `1`=right, `2`=middle,
///               `3`/`4`=X1/X2). Range `0..ZANNA_MOUSE_BUTTON_MAX-1`.
void rt_mouse_button_down(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return;

    if (!g_mouse_button_state[button]) {
        g_mouse_button_state[button] = true;
        g_mouse_button_pressed[button] = true;
        g_mouse_press_time[button] = get_time_ms();
    }
}

/// @brief Forward an OS mouse-button-release event into the runtime state.
///
/// Called by `rt_canvas_poll` for every `VGFX_EVENT_MOUSE_UP`. Records
/// the release in the per-frame arrays, then evaluates the press
/// duration to detect a click (release within `CLICK_MAX_DURATION_MS`
/// of the matching press) and a double-click (click within
/// `DOUBLE_CLICK_MAX_INTERVAL_MS` of the previous click on the same
/// button).
///
/// Out-of-range button indices are silently ignored.
///
/// @param button Mouse button index that was released.
void rt_mouse_button_up(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return;

    if (g_mouse_button_state[button]) {
        g_mouse_button_state[button] = false;
        g_mouse_button_released[button] = true;

        // Check for click (quick press and release)
        int64_t now = get_time_ms();
        int64_t press_time = g_mouse_press_time[button];
        if (now >= press_time && now - press_time <= CLICK_MAX_DURATION_MS) {
            g_mouse_clicked[button] = true;

            // Check for double-click
            int64_t last_click_time = g_mouse_last_click_time[button];
            if (last_click_time >= 0 && now >= last_click_time &&
                now - last_click_time <= DOUBLE_CLICK_MAX_INTERVAL_MS) {
                g_mouse_double_clicked[button] = true;
            }
            g_mouse_last_click_time[button] = now;
        }
    }
}

/// @brief Forward an OS scroll-wheel event into the runtime state.
///
/// Accumulates wheel deltas across all events received within a single
/// frame; `Mouse.WheelX`/`WheelY` queries return the sum, then
/// `begin_frame` resets the accumulator. Both axes are exposed so
/// horizontal scroll wheels and trackpad two-finger scroll work.
///
/// @param dx Horizontal wheel delta (positive = right). Units are
///           platform-defined "ticks" — typically integer click counts
///           on traditional wheels, fractional on touchpad scroll.
/// @param dy Vertical wheel delta (positive = up).
void rt_mouse_update_wheel(double dx, double dy) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_wheel_x = rt_input_saturating_add_f64(g_mouse_wheel_x, dx);
    g_mouse_wheel_y = rt_input_saturating_add_f64(g_mouse_wheel_y, dy);
}

/// @brief Bind the mouse to a specific Canvas window.
///
/// All subsequent `update_pos` / `button_down` / etc. calls are
/// associated with this canvas. Called automatically when a Canvas is
/// created or `begin_frame`'d. Auto-initializes the mouse subsystem on
/// the first non-NULL bind.
///
/// Pass NULL to release the binding when the canvas is destroyed (or
/// use the more conservative `rt_mouse_clear_canvas_if_matches` to
/// only clear when the binding matches).
///
/// @param canvas Canvas handle, or NULL.
void rt_mouse_set_canvas(void *canvas) {
    RT_ASSERT_MAIN_THREAD();
    if (canvas)
        rt_mouse_init();
    g_mouse_canvas = canvas;
}

/// @brief Conditionally release the mouse's canvas binding.
///
/// Mirror of `rt_keyboard_clear_canvas_if_matches` for the mouse side.
/// Called from the canvas destruction path so the global mouse state
/// won't hold a dangling pointer to a freed window. Only clears when
/// the binding matches; harmless when the mouse is bound to a different
/// canvas in a multi-canvas application.
///
/// Safe with NULL canvas (no-op).
///
/// @param canvas Canvas being destroyed.
void rt_mouse_clear_canvas_if_matches(void *canvas) {
    RT_ASSERT_MAIN_THREAD();
    if (canvas && g_mouse_canvas == canvas)
        g_mouse_canvas = NULL;
}

/// @brief Cancel every held input edge when the active window loses focus.
/// @details Platform backends clear their own state before publishing FOCUS_LOST. The runtime
///          keeps a separate snapshot, so it must be cleared as well or the next physical press
///          is filtered as a repeat of a button/key that is no longer held. Focus cancellation
///          intentionally publishes no release/click edges: losing focus must never activate a
///          control, and gesture owners can detect cancellation from the cleared level state.
void rt_input_focus_lost(void) {
    RT_ASSERT_MAIN_THREAD();

    memset(g_key_state, 0, sizeof(g_key_state));
    memset(g_pressed_this_frame, 0, sizeof(g_pressed_this_frame));
    memset(g_released_this_frame, 0, sizeof(g_released_this_frame));
    g_pressed_count = 0;
    g_released_count = 0;
    g_text_length = 0;

    for (int button = 0; button < ZANNA_MOUSE_BUTTON_MAX; ++button) {
        g_mouse_button_state[button] = false;
        g_mouse_button_pressed[button] = false;
        g_mouse_button_released[button] = false;
        g_mouse_press_time[button] = 0;
        g_mouse_last_click_time[button] = -1;
        g_mouse_clicked[button] = false;
        g_mouse_double_clicked[button] = false;
    }
    g_mouse_wheel_x = 0.0;
    g_mouse_wheel_y = 0.0;
    g_mouse_prev_x = g_mouse_x;
    g_mouse_prev_y = g_mouse_y;
    g_mouse_delta_x = 0;
    g_mouse_delta_y = 0;
    g_mouse_delta_fx = 0.0;
    g_mouse_delta_fy = 0.0;
}

//=============================================================================
// Position Methods
//=============================================================================

/// @brief Get the current pointer x position in canvas pixels.
/// @return Pointer x coordinate (top-left origin, +X right).
int64_t rt_mouse_x(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_x;
}

/// @brief Get the current pointer y position in canvas pixels.
/// @return Pointer y coordinate (top-left origin, +Y down).
int64_t rt_mouse_y(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_y;
}

/// @brief Get the per-frame pointer delta along x.
///
/// Refreshed by `rt_mouse_finalize_frame` after the poll pumps this frame's
/// events (or forced by the relative-mode overrides), so it describes the same
/// frame as `rt_mouse_x`. Stable for the rest of the frame once polling ends.
///
/// @return Movement delta in canvas pixels since the last frame (+X right).
int64_t rt_mouse_delta_x(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_delta_x;
}

/// @brief Get the per-frame pointer delta along y.
/// @return Movement delta in canvas pixels since the last frame (+Y down).
int64_t rt_mouse_delta_y(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_delta_y;
}

//=============================================================================
// Button State (Polling)
//=============================================================================

/// @brief Test whether a mouse button is currently held down (level
///        detection — true every frame while held).
///
/// Out-of-range button indices are silently treated as "not down" and
/// return `0`.
///
/// @param button Mouse button index (`0`=left, `1`=right, `2`=middle, etc.).
/// @return `1` if the button is currently held, `0` otherwise.
int8_t rt_mouse_is_down(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return 0;
    return g_mouse_button_state[button] ? 1 : 0;
}

/// @brief Test whether a mouse button is currently released (inverse of
///        `IsDown`).
///
/// Out-of-range button indices are treated as "released" and return `1`,
/// since asking about a non-existent button it can never be down.
///
/// @param button Mouse button index.
/// @return `1` if the button is currently up, `0` if down.
int8_t rt_mouse_is_up(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return 1;
    return g_mouse_button_state[button] ? 0 : 1;
}

/// @brief Convenience query for `IsDown(left)`.
/// @return `1` if the left mouse button is held, `0` otherwise.
int8_t rt_mouse_left(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_mouse_is_down(ZANNA_MOUSE_BUTTON_LEFT);
}

/// @brief Convenience query for `IsDown(right)`.
/// @return `1` if the right mouse button is held, `0` otherwise.
int8_t rt_mouse_right(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_mouse_is_down(ZANNA_MOUSE_BUTTON_RIGHT);
}

/// @brief Convenience query for `IsDown(middle)`.
/// @return `1` if the middle mouse button is held, `0` otherwise.
int8_t rt_mouse_middle(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_mouse_is_down(ZANNA_MOUSE_BUTTON_MIDDLE);
}

//=============================================================================
// Button Events (Since Last Poll)
//=============================================================================

/// @brief Edge-detect: was this button newly pressed during the current
///        frame?
///
/// True for exactly one frame when a button transitions from up to
/// down. Use for one-shot input (jump triggers, fire-once weapons,
/// menu confirm). Compare with `IsDown` for level-triggered
/// (continuous) input like movement.
///
/// @param button Mouse button index.
/// @return `1` if the button was pressed this frame, `0` otherwise.
int8_t rt_mouse_was_pressed(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return 0;
    return g_mouse_button_pressed[button] ? 1 : 0;
}

/// @brief Edge-detect: was this button newly released during the
///        current frame?
///
/// True for exactly one frame on the down-to-up transition. Use for
/// charge-attack release, drag-end detection.
///
/// @param button Mouse button index.
/// @return `1` if the button was released this frame, `0` otherwise.
int8_t rt_mouse_was_released(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return 0;
    return g_mouse_button_released[button] ? 1 : 0;
}

/// @brief Did this button complete a click (quick press + release)
///        during the current frame?
///
/// True when both the press and the release of a button happened
/// within `CLICK_MAX_DURATION_MS` of each other. Long holds don't
/// register as clicks. Recorded in `rt_mouse_button_up` at release time.
///
/// @param button Mouse button index.
/// @return `1` if a click was registered this frame, `0` otherwise.
int8_t rt_mouse_was_clicked(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return 0;
    return g_mouse_clicked[button] ? 1 : 0;
}

/// @brief Did this button complete a double-click during the current
///        frame?
///
/// True when a click occurred within `DOUBLE_CLICK_MAX_INTERVAL_MS` of
/// the previous click on the same button. Both the click and the
/// double-click flags are set on the second click; consumers can
/// branch on whichever they care about.
///
/// @param button Mouse button index.
/// @return `1` if a double-click was registered this frame, `0` otherwise.
int8_t rt_mouse_was_double_clicked(int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (button < 0 || button >= ZANNA_MOUSE_BUTTON_MAX)
        return 0;
    return g_mouse_double_clicked[button] ? 1 : 0;
}

//=============================================================================
// Scroll Wheel
//=============================================================================

/// @brief Get the integer-truncated horizontal scroll delta for this
///        frame. Use the `xf` variant if you need fractional deltas
///        from trackpad scrolling.
///
/// @return Sum of `update_wheel(dx, _)` events received this frame,
///         truncated to int64.
int64_t rt_mouse_wheel_x(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_input_saturating_trunc_f64_to_i64(g_mouse_wheel_x);
}

/// @brief Get the integer-truncated vertical scroll delta for this frame.
/// @return Sum of `update_wheel(_, dy)` events received this frame,
///         truncated to int64.
int64_t rt_mouse_wheel_y(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_input_saturating_trunc_f64_to_i64(g_mouse_wheel_y);
}

/// @brief Get the fractional horizontal scroll delta for this frame.
///
/// Preferred over `wheel_x` for smooth-scroll input devices (trackpads,
/// high-resolution mice). Reset to zero by `begin_frame`.
///
/// @return Sum of horizontal wheel deltas received this frame, as a
///         double.
double rt_mouse_wheel_xf(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_wheel_x;
}

/// @brief Get the fractional vertical scroll delta for this frame.
/// @return Sum of vertical wheel deltas received this frame, as a double.
double rt_mouse_wheel_yf(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_wheel_y;
}

//=============================================================================
// Cursor Control
//=============================================================================

/// @brief Request platform cursor visibility.
extern void vgfx_show_cursor(void);
/// @brief Request platform cursor hiding.
extern void vgfx_hide_cursor(void);

/// @brief Show the OS cursor (idempotent).
///
/// Calls `vgfx_show_cursor` exactly once on the down-to-up `hidden`
/// transition; further calls when the cursor is already visible are a
/// no-op. Use to restore the cursor after a `Mouse.Hide` call (or
/// implicitly via `Mouse.Release`).
void rt_mouse_show(void) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_mouse_hidden)
        return;
    g_mouse_hidden = false;
    vgfx_show_cursor();
}

/// @brief Hide the OS cursor (idempotent).
///
/// Calls `vgfx_hide_cursor` exactly once on the up-to-down `hidden`
/// transition. Useful for FPS-style mouse-look where the cursor would
/// otherwise be visible drifting around.
void rt_mouse_hide(void) {
    RT_ASSERT_MAIN_THREAD();
    if (g_mouse_hidden)
        return;
    g_mouse_hidden = true;
    vgfx_hide_cursor();
}

/// @brief Query whether the cursor is currently hidden.
/// @return `1` when hidden via `rt_mouse_hide` or `rt_mouse_capture`,
///         `0` otherwise.
int8_t rt_mouse_is_hidden(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_hidden ? 1 : 0;
}

/// @brief Capture the mouse: hide the cursor and mark it as captured.
///
/// "Captured" is a state flag the game can query (`IsCaptured`). The
/// runtime currently only ties it to the cursor visibility — extending
/// to true OS-level pointer warping (relative-only motion) is left to
/// the platform backend.
void rt_mouse_capture(void) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_captured = true;
    rt_mouse_hide(); /* Hide cursor during capture */
}

/// @brief Release a captured mouse: show the cursor and clear the flag.
///
/// Inverse of `rt_mouse_capture`. Safe to call when the mouse is not
/// captured.
void rt_mouse_release(void) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_captured = false;
    rt_mouse_show(); /* Restore cursor on release */
}

/// @brief Query whether the mouse is currently in captured mode.
/// @return `1` when captured, `0` otherwise.
int8_t rt_mouse_is_captured(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_captured ? 1 : 0;
}

/// @brief Request or release relative (raw) mouse mode.
///
/// Enabling implies capture (cursor hidden, absolute position frozen at the
/// capture point); disabling releases capture. The actual platform raw-input
/// mode is applied by the Canvas3D poll (which owns the window handle) and
/// reported back through `rt_mouse_set_relative_native`; until then the
/// existing warp-to-center capture path serves the deltas, so mouse-look is
/// correct either way.
/// @param enabled Non-zero to request relative mode, zero to release it.
void rt_mouse_set_relative_mode(int8_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    bool want = enabled != 0;
    if (g_mouse_relative_requested == want)
        return;
    g_mouse_relative_requested = want;
    if (want) {
        if (!g_mouse_captured)
            rt_mouse_capture();
    } else {
        g_mouse_relative_native = false;
        if (g_mouse_captured)
            rt_mouse_release();
    }
}

/// @brief Whether relative (raw) mouse mode has been requested.
/// @return One when relative mode is requested, otherwise zero.
int8_t rt_mouse_get_relative_mode(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_relative_requested ? 1 : 0;
}

/// @brief Record whether the platform window delivers native raw deltas.
///
/// Called by the Canvas3D poll after applying `vgfx_set_relative_mouse` so
/// diagnostics (`Mouse.RelativeModeNative`) reflect the truth: `0` means the
/// warp-to-center fallback is serving the deltas.
/// @param native Non-zero when the active backend supplies native raw motion.
void rt_mouse_set_relative_native(int8_t native) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_relative_native = native != 0;
}

/// @brief Whether native raw deltas are currently active.
/// @return One only when relative mode is requested and native raw input is active.
int8_t rt_mouse_get_relative_native(void) {
    RT_ASSERT_MAIN_THREAD();
    return (g_mouse_relative_requested && g_mouse_relative_native) ? 1 : 0;
}

/// @brief Sub-pixel horizontal mouse delta for the current frame.
/// @return Horizontal delta in logical canvas pixels.
double rt_mouse_delta_xf(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_delta_fx;
}

/// @brief Sub-pixel vertical mouse delta for the current frame.
/// @return Vertical delta in logical canvas pixels.
double rt_mouse_delta_yf(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_mouse_delta_fy;
}

/// @brief Move the OS cursor to the given canvas pixel position and
///        sync the runtime's tracked coordinates.
///
/// Implements `Mouse.SetPos`: updates the internal `g_mouse_x`/`y`
/// state immediately so subsequent `Mouse.X`/`Y` queries see the new
/// position even before the next OS event arrives, then warps the OS
/// cursor via the platform bridge (`vgfx_warp_cursor`) or the test hook.
///
/// @param x Target x in canvas pixels.
/// @param y Target y in canvas pixels.
void rt_mouse_set_pos(int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    g_mouse_x = x;
    g_mouse_y = y;
    rt_input_warp_mouse_platform(x, y);
}

//=============================================================================
// Button Constant Getters
//
// Match the keyboard constant getters in style — return the canonical
// ZANNA_MOUSE_BUTTON_* int64 code so Zia/BASIC programs can write
// `Mouse.IsDown(Mouse.Button.Left)` instead of magic integers.
//=============================================================================

/// @brief Button-code constant for the left mouse button.
/// @return `ZANNA_MOUSE_BUTTON_LEFT`.
int64_t rt_mouse_button_left(void) {
    return ZANNA_MOUSE_BUTTON_LEFT;
}

/// @brief Button-code constant for the right mouse button.
/// @return `ZANNA_MOUSE_BUTTON_RIGHT`.
int64_t rt_mouse_button_right(void) {
    return ZANNA_MOUSE_BUTTON_RIGHT;
}

/// @brief Button-code constant for the middle (wheel-click) mouse button.
/// @return `ZANNA_MOUSE_BUTTON_MIDDLE`.
int64_t rt_mouse_button_middle(void) {
    return ZANNA_MOUSE_BUTTON_MIDDLE;
}

/// @brief Button-code constant for the X1 / "back" extended mouse button
///        (commonly the lower thumb button on 5-button mice).
/// @return `ZANNA_MOUSE_BUTTON_X1`.
int64_t rt_mouse_button_x1(void) {
    return ZANNA_MOUSE_BUTTON_X1;
}

/// @brief Button-code constant for the X2 / "forward" extended mouse button.
/// @return `ZANNA_MOUSE_BUTTON_X2`.
int64_t rt_mouse_button_x2(void) {
    return ZANNA_MOUSE_BUTTON_X2;
}
