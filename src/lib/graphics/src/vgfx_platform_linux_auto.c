//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_linux_auto.c
// Purpose: Select and dispatch the Linux Wayland or X11 graphics adapter at runtime.
// Key invariants:
//   - The first successful window fixes one backend for the process lifetime.
//   - Backend publication uses release/acquire ordering so lock-free dispatch
//     never races first-window initialization.
// Ownership/Lifetime:
//   - Owns only process-lifetime selection state; adapters own native objects.
//   - The selection atomic remains valid until process exit.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Runtime selector and dispatch layer for Linux graphics adapters.
/// @details Chooses Wayland or X11 during first-window initialization, publishes
///          the successful choice atomically, and forwards the complete
///          ZannaGFX platform surface to that adapter for the process lifetime.

#include "vgfx_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/// @brief Process-wide Linux graphics adapter selection state.
typedef enum {
    VGFX_LINUX_BACKEND_UNSELECTED = 0,
    VGFX_LINUX_BACKEND_WAYLAND,
    VGFX_LINUX_BACKEND_X11
} vgfx_linux_backend_t;

static pthread_mutex_t g_backend_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_backend = ATOMIC_VAR_INIT(VGFX_LINUX_BACKEND_UNSELECTED);

/// @brief Declare one symbol-prefixed Linux adapter surface.
/// @details The build renames each adapter implementation through prefix
///          headers, allowing both to coexist while this translation unit
///          exports the unprefixed public platform ABI.
/// @param prefix Adapter symbol prefix such as `vgfx_wayland` or `vgfx_x11`.
#define DECLARE_ADAPTER(prefix)                                                                    \
    void *prefix##_vgfx_platform_aligned_alloc(size_t, size_t);                                    \
    void prefix##_vgfx_platform_aligned_free(void *);                                              \
    int64_t prefix##_vgfx_platform_now_ms(void);                                                   \
    void prefix##_vgfx_platform_sleep_ms(int32_t);                                                 \
    void prefix##_vgfx_platform_yield(void);                                                       \
    float prefix##_vgfx_platform_get_display_scale(void);                                          \
    int prefix##_vgfx_platform_get_display_logical_size(int32_t *, int32_t *);                     \
    int prefix##_vgfx_platform_init_window(struct vgfx_window *, const vgfx_window_params_t *);    \
    void prefix##_vgfx_platform_destroy_window(struct vgfx_window *);                              \
    int prefix##_vgfx_platform_wait_events(struct vgfx_window *, int32_t);                         \
    int prefix##_vgfx_platform_wake_events(struct vgfx_window *);                                 \
    int prefix##_vgfx_platform_process_events(struct vgfx_window *);                               \
    int prefix##_vgfx_platform_present(struct vgfx_window *);                                      \
    void prefix##_vgfx_platform_set_title(struct vgfx_window *, const char *);                     \
    void prefix##_vgfx_platform_set_icon(struct vgfx_window *, const uint32_t *, int32_t, int32_t); \
    int prefix##_vgfx_platform_set_fullscreen(struct vgfx_window *, int);                          \
    int prefix##_vgfx_platform_is_fullscreen(struct vgfx_window *);                                \
    void prefix##_vgfx_platform_minimize(struct vgfx_window *);                                    \
    void prefix##_vgfx_platform_maximize(struct vgfx_window *);                                    \
    void prefix##_vgfx_platform_restore(struct vgfx_window *);                                     \
    int32_t prefix##_vgfx_platform_is_minimized(struct vgfx_window *);                             \
    int32_t prefix##_vgfx_platform_is_maximized(struct vgfx_window *);                             \
    void prefix##_vgfx_platform_get_position(struct vgfx_window *, int32_t *, int32_t *);          \
    void prefix##_vgfx_platform_set_position(struct vgfx_window *, int32_t, int32_t);              \
    void prefix##_vgfx_platform_focus(struct vgfx_window *);                                       \
    void prefix##_vgfx_platform_request_foreground(struct vgfx_window *);                          \
    int32_t prefix##_vgfx_platform_is_focused(struct vgfx_window *);                               \
    void prefix##_vgfx_platform_set_prevent_close(struct vgfx_window *, int32_t);                  \
    void prefix##_vgfx_platform_set_cursor(struct vgfx_window *, int32_t);                         \
    void prefix##_vgfx_platform_set_cursor_visible(struct vgfx_window *, int32_t);                 \
    void prefix##_vgfx_platform_hide_cursor(void);                                                 \
    void prefix##_vgfx_platform_show_cursor(void);                                                 \
    void prefix##_vgfx_platform_get_monitor_size(struct vgfx_window *, int32_t *, int32_t *);      \
    void prefix##_vgfx_platform_set_window_size(struct vgfx_window *, int32_t, int32_t);           \
    void prefix##_vgfx_platform_set_window_min_size(struct vgfx_window *, int32_t, int32_t);       \
    int prefix##_vgfx_clipboard_has_format(vgfx_clipboard_format_t);                               \
    char *prefix##_vgfx_clipboard_get_text(void);                                                  \
    void prefix##_vgfx_clipboard_set_text(const char *);                                           \
    void prefix##_vgfx_clipboard_clear(void);                                                      \
    void *prefix##_vgfx_get_native_view(vgfx_window_t);                                            \
    void *prefix##_vgfx_get_native_display(vgfx_window_t);                                         \
    int prefix##_vgfx_get_native_handles(vgfx_window_t, vgfx_native_handles_t *);                  \
    vgfx_window_capabilities_t prefix##_vgfx_get_window_capabilities(vgfx_window_t);               \
    void prefix##_vgfx_platform_warp_cursor(vgfx_window_t, int32_t, int32_t);                      \
    int prefix##_vgfx_platform_set_relative_mouse(struct vgfx_window *, int);                      \
    int prefix##_vgfx_platform_set_text_input_enabled(struct vgfx_window *, int32_t);              \
    int prefix##_vgfx_platform_set_text_input_state(struct vgfx_window *,                          \
                                                    const vgfx_text_input_state_t *)

DECLARE_ADAPTER(vgfx_wayland);
#if defined(VGFX_AUTO_HAS_X11)
DECLARE_ADAPTER(vgfx_x11);
#endif

/// @brief Test whether an environment variable has a non-empty value.
/// @param name Environment-variable name.
/// @return 1 when present and non-empty, otherwise 0.
static int env_has_value(const char *name) {
    const char *value = getenv(name);
    return value && value[0] != '\0';
}

/// @brief Read the published backend or infer dispatch preference before selection.
/// @details An acquire load observes the backend published by successful
///          initialization.  Before publication, a non-empty WAYLAND_DISPLAY
///          prefers Wayland; otherwise X11 is preferred when compiled in.
/// @return Published or environment-preferred backend.
static vgfx_linux_backend_t preferred_backend(void) {
    const vgfx_linux_backend_t selected =
        (vgfx_linux_backend_t)atomic_load_explicit(&g_backend, memory_order_acquire);
    if (selected != VGFX_LINUX_BACKEND_UNSELECTED)
        return selected;
    return env_has_value("WAYLAND_DISPLAY") ? VGFX_LINUX_BACKEND_WAYLAND
#if defined(VGFX_AUTO_HAS_X11)
                                            : VGFX_LINUX_BACKEND_X11;
#else
                                            : VGFX_LINUX_BACKEND_WAYLAND;
#endif
}

/// @brief Initialize a window and permanently select the first successful Linux adapter.
/// @details Serializes first-window selection.  An already selected adapter is
///          reused directly.  Otherwise Wayland is attempted when its display
///          variable is present, then compiled-in X11 when DISPLAY is present.
///          Success is release-published for lock-free subsequent dispatch;
///          total failure leaves the backend unselected and reports a platform
///          error when no usable fallback remains.
/// @param win Window whose native resources should be initialized.
/// @param params Validated creation parameters.
/// @return 1 when the selected adapter created the window, otherwise 0.
int vgfx_platform_init_window(struct vgfx_window *win, const vgfx_window_params_t *params) {
    int result = 0;
    (void)pthread_mutex_lock(&g_backend_mutex);
    const vgfx_linux_backend_t selected =
        (vgfx_linux_backend_t)atomic_load_explicit(&g_backend, memory_order_relaxed);
    if (selected == VGFX_LINUX_BACKEND_WAYLAND)
        result = vgfx_wayland_vgfx_platform_init_window(win, params);
#if defined(VGFX_AUTO_HAS_X11)
    else if (selected == VGFX_LINUX_BACKEND_X11)
        result = vgfx_x11_vgfx_platform_init_window(win, params);
#endif
    else {
        if (env_has_value("WAYLAND_DISPLAY") &&
            vgfx_wayland_vgfx_platform_init_window(win, params)) {
            atomic_store_explicit(&g_backend, VGFX_LINUX_BACKEND_WAYLAND, memory_order_release);
            result = 1;
        }
#if defined(VGFX_AUTO_HAS_X11)
        else if (env_has_value("DISPLAY") && vgfx_x11_vgfx_platform_init_window(win, params)) {
            atomic_store_explicit(&g_backend, VGFX_LINUX_BACKEND_X11, memory_order_release);
            result = 1;
        }
#endif
        else if (!env_has_value("WAYLAND_DISPLAY")) {
            vgfx_internal_set_error(VGFX_ERR_PLATFORM,
                                    "no usable Linux display: WAYLAND_DISPLAY is unset"
#if defined(VGFX_AUTO_HAS_X11)
                                    " and DISPLAY is unset or X11 initialization failed"
#else
                                    " and this build has no X11 adapter"
#endif
            );
        }
    }
    (void)pthread_mutex_unlock(&g_backend_mutex);
    return result;
}

/// @brief Define a value-returning dispatch wrapper for the selected adapter.
/// @param type Wrapper return type.
/// @param name Unprefixed platform function name.
/// @param params Parenthesized wrapper parameter declaration.
/// @param args Parenthesized adapter-call argument list.
/// @param fallback Value returned when no compiled adapter branch is available.
#define DISPATCH_RET(type, name, params, args, fallback)                                           \
    type name params {                                                                             \
        if (preferred_backend() == VGFX_LINUX_BACKEND_WAYLAND)                                     \
            return vgfx_wayland_##name args;                                                       \
        (void)0;                                                                                   \
        /* X11 is compiled only when its development surface was found. */                         \
        VGFX_AUTO_X11_RETURN(name, args)                                                           \
        return fallback;                                                                           \
    }

/// @brief Define a void dispatch wrapper for the selected adapter.
/// @param name Unprefixed platform function name.
/// @param params Parenthesized wrapper parameter declaration.
/// @param args Parenthesized adapter-call argument list.
#define DISPATCH_VOID(name, params, args)                                                          \
    void name params {                                                                             \
        if (preferred_backend() == VGFX_LINUX_BACKEND_WAYLAND) {                                   \
            vgfx_wayland_##name args;                                                              \
            return;                                                                                \
        }                                                                                          \
        VGFX_AUTO_X11_CALL(name, args)                                                             \
    }

#if defined(VGFX_AUTO_HAS_X11)
#define VGFX_AUTO_X11_RETURN(name, args) return vgfx_x11_##name args;
#define VGFX_AUTO_X11_CALL(name, args) vgfx_x11_##name args;
#else
#define VGFX_AUTO_X11_RETURN(name, args)
#define VGFX_AUTO_X11_CALL(name, args)
#endif

/// @copydoc vgfx_platform_aligned_alloc
DISPATCH_RET(void *, vgfx_platform_aligned_alloc, (size_t a, size_t s), (a, s), NULL)
/// @copydoc vgfx_platform_aligned_free
DISPATCH_VOID(vgfx_platform_aligned_free, (void *p), (p))
/// @copydoc vgfx_platform_now_ms
DISPATCH_RET(int64_t, vgfx_platform_now_ms, (void), (), 0)
/// @copydoc vgfx_platform_sleep_ms
DISPATCH_VOID(vgfx_platform_sleep_ms, (int32_t ms), (ms))
/// @copydoc vgfx_platform_yield
DISPATCH_VOID(vgfx_platform_yield, (void), ())
/// @copydoc vgfx_platform_get_display_scale
DISPATCH_RET(float, vgfx_platform_get_display_scale, (void), (), 1.0f)
/// @brief Query logical dimensions through the selected Linux adapter.
/// @param w Receives logical display width when non-NULL.
/// @param h Receives logical display height when non-NULL.
/// @return 1 on success, or 0 when no selected adapter can answer.
DISPATCH_RET(int, vgfx_platform_get_display_logical_size, (int32_t *w, int32_t *h), (w, h), 0)
/// @copydoc vgfx_platform_destroy_window
DISPATCH_VOID(vgfx_platform_destroy_window, (struct vgfx_window * w), (w))
/// @copydoc vgfx_platform_wait_events
DISPATCH_RET(int, vgfx_platform_wait_events, (struct vgfx_window * w, int32_t ms), (w, ms), 0)
/// @copydoc vgfx_platform_wake_events
DISPATCH_RET(int, vgfx_platform_wake_events, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_process_events
DISPATCH_RET(int, vgfx_platform_process_events, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_present
DISPATCH_RET(int, vgfx_platform_present, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_set_title
DISPATCH_VOID(vgfx_platform_set_title, (struct vgfx_window * w, const char *s), (w, s))
/// @copydoc vgfx_platform_set_icon
DISPATCH_VOID(vgfx_platform_set_icon,
              (struct vgfx_window * w, const uint32_t *p, int32_t cw, int32_t ch),
              (w, p, cw, ch))
/// @copydoc vgfx_platform_set_fullscreen
DISPATCH_RET(int, vgfx_platform_set_fullscreen, (struct vgfx_window * w, int v), (w, v), 0)
/// @copydoc vgfx_platform_is_fullscreen
DISPATCH_RET(int, vgfx_platform_is_fullscreen, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_minimize
DISPATCH_VOID(vgfx_platform_minimize, (struct vgfx_window * w), (w))
/// @copydoc vgfx_platform_maximize
DISPATCH_VOID(vgfx_platform_maximize, (struct vgfx_window * w), (w))
/// @copydoc vgfx_platform_restore
DISPATCH_VOID(vgfx_platform_restore, (struct vgfx_window * w), (w))
/// @copydoc vgfx_platform_is_minimized
DISPATCH_RET(int32_t, vgfx_platform_is_minimized, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_is_maximized
DISPATCH_RET(int32_t, vgfx_platform_is_maximized, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_get_position
DISPATCH_VOID(vgfx_platform_get_position,
              (struct vgfx_window * w, int32_t *x, int32_t *y),
              (w, x, y))
/// @copydoc vgfx_platform_set_position
DISPATCH_VOID(vgfx_platform_set_position, (struct vgfx_window * w, int32_t x, int32_t y), (w, x, y))
/// @copydoc vgfx_platform_focus
DISPATCH_VOID(vgfx_platform_focus, (struct vgfx_window * w), (w))
/// @copydoc vgfx_platform_request_foreground
DISPATCH_VOID(vgfx_platform_request_foreground, (struct vgfx_window * w), (w))
/// @copydoc vgfx_platform_is_focused
DISPATCH_RET(int32_t, vgfx_platform_is_focused, (struct vgfx_window * w), (w), 0)
/// @copydoc vgfx_platform_set_prevent_close
DISPATCH_VOID(vgfx_platform_set_prevent_close, (struct vgfx_window * w, int32_t v), (w, v))
/// @copydoc vgfx_platform_set_cursor
DISPATCH_VOID(vgfx_platform_set_cursor, (struct vgfx_window * w, int32_t v), (w, v))
/// @copydoc vgfx_platform_set_cursor_visible
DISPATCH_VOID(vgfx_platform_set_cursor_visible, (struct vgfx_window * w, int32_t v), (w, v))
/// @copydoc vgfx_platform_hide_cursor
DISPATCH_VOID(vgfx_platform_hide_cursor, (void), ())
/// @copydoc vgfx_platform_show_cursor
DISPATCH_VOID(vgfx_platform_show_cursor, (void), ())
/// @copydoc vgfx_platform_get_monitor_size
DISPATCH_VOID(vgfx_platform_get_monitor_size,
              (struct vgfx_window * w, int32_t *x, int32_t *y),
              (w, x, y))
/// @copydoc vgfx_platform_set_window_size
DISPATCH_VOID(vgfx_platform_set_window_size,
              (struct vgfx_window * w, int32_t x, int32_t y),
              (w, x, y))
/// @copydoc vgfx_platform_set_window_min_size
DISPATCH_VOID(vgfx_platform_set_window_min_size,
              (struct vgfx_window * w, int32_t x, int32_t y),
              (w, x, y))
/// @copydoc vgfx_clipboard_has_format
DISPATCH_RET(int, vgfx_clipboard_has_format, (vgfx_clipboard_format_t f), (f), 0)
/// @copydoc vgfx_clipboard_get_text
DISPATCH_RET(char *, vgfx_clipboard_get_text, (void), (), NULL)
/// @copydoc vgfx_clipboard_set_text
DISPATCH_VOID(vgfx_clipboard_set_text, (const char *s), (s))
/// @copydoc vgfx_clipboard_clear
DISPATCH_VOID(vgfx_clipboard_clear, (void), ())
/// @copydoc vgfx_get_native_view
DISPATCH_RET(void *, vgfx_get_native_view, (vgfx_window_t w), (w), NULL)
/// @copydoc vgfx_get_native_display
DISPATCH_RET(void *, vgfx_get_native_display, (vgfx_window_t w), (w), NULL)
/// @copydoc vgfx_get_native_handles
DISPATCH_RET(int, vgfx_get_native_handles, (vgfx_window_t w, vgfx_native_handles_t *h), (w, h), 0)
/// @copydoc vgfx_get_window_capabilities
DISPATCH_RET(vgfx_window_capabilities_t, vgfx_get_window_capabilities, (vgfx_window_t w), (w), 0)
/// @brief Warp the pointer through the selected Linux adapter.
/// @param w Target window.
/// @param x Logical client X coordinate.
/// @param y Logical client Y coordinate.
DISPATCH_VOID(vgfx_platform_warp_cursor, (vgfx_window_t w, int32_t x, int32_t y), (w, x, y))
/// @brief Toggle native relative mouse support through the selected adapter.
/// @param w Target window.
/// @param v Non-zero to enable, zero to disable.
/// @return 1 when the adapter applied native relative mode, otherwise 0.
DISPATCH_RET(int, vgfx_platform_set_relative_mouse, (struct vgfx_window * w, int v), (w, v), 0)
/// @copydoc vgfx_platform_set_text_input_enabled
DISPATCH_RET(
    int, vgfx_platform_set_text_input_enabled, (struct vgfx_window * w, int32_t v), (w, v), 0)
/// @copydoc vgfx_platform_set_text_input_state
DISPATCH_RET(int,
             vgfx_platform_set_text_input_state,
             (struct vgfx_window * w, const vgfx_text_input_state_t *s),
             (w, s),
             0)
