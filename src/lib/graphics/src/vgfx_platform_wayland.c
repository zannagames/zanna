//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_wayland.c
// Purpose: Native Linux Wayland windowing and software-presentation backend.
// Key invariants:
//   - No Wayland headers or link-time Wayland dependency are required.
//   - Protocol events are dispatched only on the window's owning connection.
// Ownership/Lifetime:
//   - Each window owns one connection, xdg shell role, and shm presenter.
//   - Clipboard and DND transfers are owned by the active window data device.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md,
//        src/lib/graphics/src/vgfx_wayland_shell.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Native Wayland windowing and software-presentation backend.
/// @details Composes the dynamically loaded Wayland connection, xdg-shell,
///          input, scale, data-device, cursor, activation, relative-pointer,
///          text-input, client-decoration, and shared-memory presenter modules
///          into the ZannaGFX platform interface without Wayland link-time
///          dependencies.

#define _POSIX_C_SOURCE 200809L

#include "vgfx_internal.h"
#include "vgfx_wayland_activation.h"
#include "vgfx_wayland_csd.h"
#include "vgfx_wayland_cursor.h"
#include "vgfx_wayland_data.h"
#include "vgfx_wayland_input.h"
#include "vgfx_wayland_relative.h"
#include "vgfx_wayland_scale.h"
#include "vgfx_wayland_shm.h"
#include "vgfx_wayland_text_input.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/// @brief Aggregate state owned by one Wayland-backed ZannaGFX window.
/// @details Owns every protocol submodule and caches the last public state
///          reported to the core so configure/focus/close changes generate
///          events exactly once.
typedef struct vgfx_wayland_platform {
    vgfx_wayland_connection_t connection;
    vgfx_wayland_activation_t activation;
    vgfx_wayland_csd_t csd;
    vgfx_wayland_shell_t shell;
    vgfx_wayland_input_t input;
    vgfx_wayland_relative_t relative;
    vgfx_wayland_data_t data;
    vgfx_wayland_cursor_t cursor;
    vgfx_wayland_scale_t scale;
    vgfx_wayland_text_input_t text_input;
    vgfx_wayland_shm_presenter_t presenter;
    int32_t reported_width;
    int32_t reported_height;
    int32_t reported_logical_width;
    int32_t reported_logical_height;
    int32_t requested_logical_width;
    int32_t requested_logical_height;
    float reported_scale;
    int32_t reported_close;
    int32_t reported_focus;
    int32_t minimized;
    int32_t cursor_type;
    int32_t cursor_visible;
    int wake_read_fd;
    int wake_write_fd;
} vgfx_wayland_platform_t;

static vgfx_wayland_data_t *g_vgfx_wayland_active_data;
static vgfx_wayland_cursor_t *g_vgfx_wayland_active_cursor;

/// @copydoc vgfx_platform_aligned_alloc
void *vgfx_platform_aligned_alloc(size_t alignment, size_t size) {
    if (size == 0)
        return NULL;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    void *result = NULL;
    return posix_memalign(&result, alignment, size) == 0 ? result : NULL;
}

/// @copydoc vgfx_platform_aligned_free
void vgfx_platform_aligned_free(void *ptr) {
    free(ptr);
}

/// @copydoc vgfx_platform_now_ms
int64_t vgfx_platform_now_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/// @copydoc vgfx_platform_sleep_ms
void vgfx_platform_sleep_ms(int32_t ms) {
    if (ms <= 0)
        return;
    struct timespec duration = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

/// @copydoc vgfx_platform_yield
void vgfx_platform_yield(void) {
    (void)sched_yield();
}

/// @brief Read the initial Wayland display scale from `GDK_SCALE`.
/// @details Accepts a complete finite numeric value in [1, 8] and otherwise
///          falls back to 1.0.  Per-window compositor scale updates later flow
///          through the scale protocol module.
/// @return Initial physical-pixels-per-logical-unit scale.
float vgfx_platform_get_display_scale(void) {
    const char *text = getenv("GDK_SCALE");
    if (!text || !text[0])
        return 1.0f;
    char *end = NULL;
    float scale = strtof(text, &end);
    return end != text && *end == '\0' && scale >= 1.0f && scale <= 8.0f ? scale : 1.0f;
}

/// @brief Report that no global logical display extent is available before connection.
/// @param out_w Ignored width output.
/// @param out_h Ignored height output.
/// @return Always 0; Wayland monitor geometry is window/output scoped.
int vgfx_platform_get_display_logical_size(int32_t *out_w, int32_t *out_h) {
    (void)out_w;
    (void)out_h;
    return 0;
}

/// @brief Publish a Wayland platform failure through the shared error channel.
/// @param message Descriptive message, or NULL for a generic fallback.
static void vgfx_wayland_set_error(const char *message) {
    vgfx_internal_set_error(VGFX_ERR_PLATFORM, message ? message : "Wayland backend failed");
}

/// @brief Create the nonblocking self-pipe used to interrupt Wayland waits.
static int vgfx_wayland_wake_pipe_open(vgfx_wayland_platform_t *platform) {
    if (!platform)
        return 0;
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0)
        return 0;
    int read_flags = fcntl(descriptors[0], F_GETFL, 0);
    int write_flags = fcntl(descriptors[1], F_GETFL, 0);
    if (read_flags < 0 || write_flags < 0 ||
        fcntl(descriptors[0], F_SETFL, read_flags | O_NONBLOCK) != 0 ||
        fcntl(descriptors[1], F_SETFL, write_flags | O_NONBLOCK) != 0 ||
        fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return 0;
    }
    platform->wake_read_fd = descriptors[0];
    platform->wake_write_fd = descriptors[1];
    return 1;
}

/// @brief Drain every coalesced byte from the Wayland wake self-pipe.
static void vgfx_wayland_wake_pipe_drain(vgfx_wayland_platform_t *platform) {
    if (!platform || platform->wake_read_fd < 0)
        return;
    char bytes[64];
    while (read(platform->wake_read_fd, bytes, sizeof(bytes)) > 0) {
    }
}

/// @brief Dispatch pending Wayland traffic with a bounded connection wait.
/// @details Follows the prepare-read/flush/poll/read protocol, drains already
///          queued callbacks, handles a blocked outgoing flush with POLLOUT,
///          retries interrupted polls, and always cancels prepared reads when
///          no incoming data is consumed.
/// @param platform Initialized window platform state.
/// @param timeout_ms Poll timeout; negative waits indefinitely.
/// @return 0 on connection/protocol failure, 1 for a clean timeout/no new
///         callback, or 2 when callbacks were dispatched.
static int vgfx_wayland_dispatch_available(vgfx_wayland_platform_t *platform, int32_t timeout_ms) {
    if (!platform || !platform->connection.display)
        return 0;
    vgfx_wayland_client_api_t *api = &platform->connection.api;
    int dispatched = 0;
    while (api->display_prepare_read(platform->connection.display) != 0) {
        int pending = api->display_dispatch_pending(platform->connection.display);
        if (pending < 0)
            return 0;
        if (pending > 0)
            dispatched = 1;
    }
    int flush_result = api->display_flush(platform->connection.display);
    int flush_blocked = flush_result < 0 && errno == EAGAIN;
    if (flush_result < 0 && !flush_blocked) {
        api->display_cancel_read(platform->connection.display);
        return 0;
    }
    struct pollfd descriptors[2] = {
        {.fd = api->display_get_fd(platform->connection.display),
         .events = (short)(POLLIN | (flush_blocked ? POLLOUT : 0)),
         .revents = 0},
        {.fd = platform->wake_read_fd, .events = POLLIN, .revents = 0}};
    nfds_t descriptor_count = platform->wake_read_fd >= 0 ? 2 : 1;
    int result;
    do {
        result = poll(descriptors, descriptor_count, timeout_ms < 0 ? -1 : timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result > 0 && (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
        api->display_cancel_read(platform->connection.display);
        return 0;
    }
    int was_woken = result > 0 && descriptor_count > 1 &&
                     (descriptors[1].revents & POLLIN) != 0;
    if (was_woken)
        vgfx_wayland_wake_pipe_drain(platform);
    if (result > 0 && (descriptors[0].revents & POLLIN)) {
        if (api->display_read_events(platform->connection.display) < 0)
            return 0;
        if (flush_blocked && (descriptors[0].revents & POLLOUT) &&
            api->display_flush(platform->connection.display) < 0 && errno != EAGAIN)
            return 0;
        return api->display_dispatch_pending(platform->connection.display) >= 0 ? 2 : 0;
    }
    api->display_cancel_read(platform->connection.display);
    if (result > 0 && flush_blocked && (descriptors[0].revents & POLLOUT) &&
        api->display_flush(platform->connection.display) < 0 && errno != EAGAIN)
        return 0;
    return result >= 0 ? (dispatched || was_woken ? 2 : 1) : 0;
}

/// @brief Reconcile compositor state with core framebuffer and public events.
/// @details Resolves configured/requested logical size and scale, safely rounds
///          physical dimensions, recreates CSD and shared-memory presentation
///          storage on change, emits resize/focus/close transitions once, and
///          clears sticky input when activation is lost.
/// @param win Window whose aggregate Wayland state should be synchronized.
/// @return 1 when synchronization succeeded, otherwise 0 with an error set
///         where appropriate.
static int vgfx_wayland_sync_state(struct vgfx_window *win) {
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    if (!platform)
        return 0;
    float scale = vgfx_wayland_scale_factor(&platform->scale);
    int32_t logical_width =
        platform->shell.width > 0 ? platform->shell.width : platform->reported_logical_width;
    int32_t logical_height =
        platform->shell.height > 0 ? platform->shell.height : platform->reported_logical_height;
    if (platform->shell.maximized || platform->shell.fullscreen || platform->shell.resizing) {
        platform->requested_logical_width = 0;
        platform->requested_logical_height = 0;
    } else if (platform->requested_logical_width > 0 && platform->requested_logical_height > 0) {
        logical_width = platform->requested_logical_width;
        logical_height = platform->requested_logical_height;
    }
    if (logical_width <= 0)
        logical_width = (int32_t)((float)win->width / (scale > 0.0f ? scale : 1.0f));
    if (logical_height <= 0)
        logical_height = (int32_t)((float)win->height / (scale > 0.0f ? scale : 1.0f));
    double scaled_width = (double)logical_width * (double)scale;
    double scaled_height = (double)logical_height * (double)scale;
    if (scaled_width > INT32_MAX || scaled_height > INT32_MAX) {
        vgfx_wayland_set_error("Wayland scaled framebuffer dimensions overflow");
        return 0;
    }
    int32_t width = (int32_t)scaled_width;
    int32_t height = (int32_t)scaled_height;
    if ((double)width < scaled_width)
        width++;
    if ((double)height < scaled_height)
        height++;
    vgfx_wayland_scale_set_logical_size(&platform->scale, logical_width, logical_height);
    if (scale < platform->reported_scale - 0.001f || scale > platform->reported_scale + 0.001f) {
        vgfx_internal_refresh_scale_factor(win, scale);
        platform->reported_scale = scale;
    }
    if (width != platform->reported_width || height != platform->reported_height) {
        vgfx_wayland_shm_close(&platform->presenter);
        if (!vgfx_internal_resize_framebuffer(win, width, height))
            return 0;
        char error[256];
        if (!vgfx_wayland_csd_resize(
                &platform->csd, logical_width, logical_height, error, sizeof(error)) ||
            !vgfx_wayland_shm_open(
                &platform->presenter, &platform->shell, width, height, error, sizeof(error))) {
            vgfx_wayland_set_error(error);
            return 0;
        }
        platform->reported_width = width;
        platform->reported_height = height;
        platform->reported_logical_width = logical_width;
        platform->reported_logical_height = logical_height;
        vgfx_event_t event;
        vgfx_internal_init_resize_event(&event, win, vgfx_platform_now_ms(), width, height);
        vgfx_internal_enqueue_event(win, &event);
    }
    if (platform->shell.activated != platform->reported_focus) {
        platform->reported_focus = platform->shell.activated;
        vgfx_internal_set_focus_state(win, platform->reported_focus);
        if (!platform->reported_focus)
            vgfx_internal_clear_input_state(win);
        else
            g_vgfx_wayland_active_data = &platform->data;
        vgfx_event_t event = {.type = platform->reported_focus ? VGFX_EVENT_FOCUS_GAINED
                                                               : VGFX_EVENT_FOCUS_LOST,
                              .time_ms = vgfx_platform_now_ms()};
        vgfx_internal_enqueue_event(win, &event);
    }
    if (platform->shell.close_requested && !platform->reported_close) {
        platform->reported_close = 1;
        if (!win->prevent_close)
            vgfx_internal_set_close_requested(win, 1);
        vgfx_event_t event = {.type = VGFX_EVENT_CLOSE, .time_ms = vgfx_platform_now_ms()};
        vgfx_internal_enqueue_event(win, &event);
    }
    return 1;
}

/// @copydoc vgfx_platform_init_window
int vgfx_platform_init_window(struct vgfx_window *win, const vgfx_window_params_t *params) {
    if (!win || !params)
        return 0;
    vgfx_wayland_platform_t *platform = calloc(1, sizeof(*platform));
    if (!platform) {
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Could not allocate Wayland window state");
        return 0;
    }
    win->platform_data = platform;
    platform->wake_read_fd = -1;
    platform->wake_write_fd = -1;
    platform->cursor_visible = 1;
    char error[256];
    if (!vgfx_wayland_wake_pipe_open(platform)) {
        vgfx_wayland_set_error("Could not create Wayland event-wake pipe");
        vgfx_platform_destroy_window(win);
        return 0;
    }
    if (!vgfx_wayland_connection_open(&platform->connection, NULL, NULL, error, sizeof(error)) ||
        !vgfx_wayland_shell_open(&platform->shell,
                                 &platform->connection,
                                 params->title,
                                 "org.zanna.app",
                                 error,
                                 sizeof(error)) ||
        !vgfx_wayland_scale_open(&platform->scale, &platform->connection, &platform->shell) ||
        !vgfx_wayland_input_open(&platform->input,
                                 &platform->connection,
                                 (struct wl_proxy *)platform->shell.surface,
                                 win,
                                 error,
                                 sizeof(error)) ||
        !vgfx_wayland_csd_open(&platform->csd,
                               &platform->shell,
                               &platform->input,
                               win,
                               params->width,
                               params->height,
                               error,
                               sizeof(error)) ||
        !vgfx_wayland_shm_open(&platform->presenter,
                               &platform->shell,
                               win->width,
                               win->height,
                               error,
                               sizeof(error))) {
        vgfx_wayland_set_error(error);
        vgfx_platform_destroy_window(win);
        return 0;
    }
    platform->reported_width = win->width;
    platform->reported_height = win->height;
    platform->reported_logical_width = params->width;
    platform->reported_logical_height = params->height;
    platform->reported_scale = vgfx_wayland_scale_factor(&platform->scale);
    vgfx_internal_refresh_scale_factor(win, platform->reported_scale);
    (void)vgfx_wayland_data_open(&platform->data, &platform->connection, &platform->input, win);
    (void)vgfx_wayland_text_input_open(
        &platform->text_input, &platform->connection, &platform->input, win);
    (void)vgfx_wayland_cursor_open(&platform->cursor, &platform->connection, &platform->input);
    vgfx_wayland_relative_init(&platform->relative,
                               &platform->connection,
                               &platform->input,
                               (struct wl_proxy *)platform->shell.surface,
                               win);
    vgfx_wayland_activation_init(&platform->activation,
                                 &platform->connection,
                                 &platform->input,
                                 (struct wl_proxy *)platform->shell.surface,
                                 "org.zanna.app");
    g_vgfx_wayland_active_data = &platform->data;
    g_vgfx_wayland_active_cursor = &platform->cursor;
    if (params->fullscreen)
        (void)vgfx_platform_set_fullscreen(win, 1);
    return 1;
}

/// @copydoc vgfx_platform_destroy_window
void vgfx_platform_destroy_window(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    vgfx_wayland_shm_close(&platform->presenter);
    if (g_vgfx_wayland_active_data == &platform->data)
        g_vgfx_wayland_active_data = NULL;
    if (g_vgfx_wayland_active_cursor == &platform->cursor)
        g_vgfx_wayland_active_cursor = NULL;
    vgfx_wayland_cursor_close(&platform->cursor);
    vgfx_wayland_activation_close(&platform->activation);
    vgfx_wayland_relative_close(&platform->relative);
    vgfx_wayland_text_input_close(&platform->text_input);
    vgfx_wayland_data_close(&platform->data);
    vgfx_wayland_csd_close(&platform->csd);
    vgfx_wayland_input_close(&platform->input);
    vgfx_wayland_scale_close(&platform->scale);
    vgfx_wayland_shell_close(&platform->shell);
    vgfx_wayland_connection_close(&platform->connection);
    if (platform->wake_read_fd >= 0)
        close(platform->wake_read_fd);
    if (platform->wake_write_fd >= 0)
        close(platform->wake_write_fd);
    platform->wake_read_fd = -1;
    platform->wake_write_fd = -1;
    free(platform);
    win->platform_data = NULL;
}

/// @copydoc vgfx_platform_wait_events
int vgfx_platform_wait_events(struct vgfx_window *win, int32_t timeout_ms) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    int64_t now = vgfx_platform_now_ms();
    int32_t clamped = vgfx_wayland_input_clamp_timeout(&platform->input, timeout_ms, now);
    int dispatched = vgfx_wayland_dispatch_available(platform, clamped);
    int repeated = vgfx_wayland_input_tick(&platform->input, vgfx_platform_now_ms());
    vgfx_wayland_data_tick(&platform->data);
    if (dispatched && !vgfx_wayland_sync_state(win))
        return 0;
    return dispatched == 2 || repeated > 0;
}

/// @copydoc vgfx_platform_wake_events
int vgfx_platform_wake_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    if (platform->wake_write_fd < 0)
        return 0;
    const unsigned char byte = 1;
    ssize_t written;
    do {
        written = write(platform->wake_write_fd, &byte, 1);
    } while (written < 0 && errno == EINTR);
    return written == 1 || (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

/// @copydoc vgfx_platform_process_events
int vgfx_platform_process_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    if (!vgfx_wayland_dispatch_available(platform, 0))
        return 0;
    (void)vgfx_wayland_input_tick(&platform->input, vgfx_platform_now_ms());
    vgfx_wayland_data_tick(&platform->data);
    return vgfx_wayland_sync_state(win);
}

/// @brief Check whether Wayland surfaces should stay unmapped.
/// @details A wl_surface with no committed buffer never maps, so skipping
///          the shm commit keeps the window invisible while frames continue
///          to render CPU-side (embedded play and headless probes), matching
///          `ZANNA_GFX_HIDE_WINDOWS` on the macOS/X11/Win32 adapters.
/// @return 1 when `ZANNA_GFX_HIDE_WINDOWS` is enabled, otherwise 0.
static int vgfx_wayland_hide_windows(void) {
    const char *value = getenv("ZANNA_GFX_HIDE_WINDOWS");
    if (!value || value[0] == '\0')
        return 0;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 && strcmp(value, "FALSE") != 0 &&
           strcmp(value, "off") != 0 && strcmp(value, "OFF") != 0;
}

/// @copydoc vgfx_platform_present
int vgfx_platform_present(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    if (win->skip_software_present)
        return 1;
    if (vgfx_wayland_hide_windows())
        return 1;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    size_t size = (size_t)win->stride * (size_t)win->height;
    return vgfx_wayland_shm_present(&platform->presenter, win->pixels, size);
}

/// @copydoc vgfx_platform_set_title
void vgfx_platform_set_title(struct vgfx_window *win, const char *title) {
    if (!win || !win->platform_data)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    const char *safe = title && title[0] ? title : "Zanna";
    (void)platform->connection.api.proxy_marshal_flags(
        (struct wl_proxy *)platform->shell.toplevel,
        VGFX_XDG_TOPLEVEL_SET_TITLE,
        NULL,
        platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
        0,
        safe);
}

/// @copydoc vgfx_platform_set_fullscreen
int vgfx_platform_set_fullscreen(struct vgfx_window *win, int fullscreen) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    uint32_t opcode =
        fullscreen ? VGFX_XDG_TOPLEVEL_SET_FULLSCREEN : VGFX_XDG_TOPLEVEL_UNSET_FULLSCREEN;
    if (fullscreen) {
        (void)platform->connection.api.proxy_marshal_flags(
            (struct wl_proxy *)platform->shell.toplevel,
            opcode,
            NULL,
            platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
            0,
            NULL);
    } else {
        (void)platform->connection.api.proxy_marshal_flags(
            (struct wl_proxy *)platform->shell.toplevel,
            opcode,
            NULL,
            platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
            0);
    }
    return platform->connection.api.display_flush(platform->connection.display) >= 0 ||
                   errno == EAGAIN
               ? 1
               : 0;
}

/// @copydoc vgfx_platform_is_fullscreen
int vgfx_platform_is_fullscreen(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    return ((vgfx_wayland_platform_t *)win->platform_data)->shell.fullscreen;
}

/// @copydoc vgfx_platform_minimize
void vgfx_platform_minimize(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    (void)platform->connection.api.proxy_marshal_flags(
        (struct wl_proxy *)platform->shell.toplevel,
        VGFX_XDG_TOPLEVEL_SET_MINIMIZED,
        NULL,
        platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
        0);
    platform->minimized = 1;
}

/// @copydoc vgfx_platform_maximize
void vgfx_platform_maximize(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    (void)platform->connection.api.proxy_marshal_flags(
        (struct wl_proxy *)platform->shell.toplevel,
        VGFX_XDG_TOPLEVEL_SET_MAXIMIZED,
        NULL,
        platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
        0);
}

/// @copydoc vgfx_platform_restore
void vgfx_platform_restore(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    (void)platform->connection.api.proxy_marshal_flags(
        (struct wl_proxy *)platform->shell.toplevel,
        VGFX_XDG_TOPLEVEL_UNSET_MAXIMIZED,
        NULL,
        platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
        0);
    platform->minimized = 0;
}

/// @copydoc vgfx_platform_is_minimized
int32_t vgfx_platform_is_minimized(struct vgfx_window *win) {
    return win && win->platform_data ? ((vgfx_wayland_platform_t *)win->platform_data)->minimized
                                     : 0;
}

/// @copydoc vgfx_platform_is_maximized
int32_t vgfx_platform_is_maximized(struct vgfx_window *win) {
    return win && win->platform_data
               ? ((vgfx_wayland_platform_t *)win->platform_data)->shell.maximized
               : 0;
}

/// @copydoc vgfx_platform_get_position
/// @details Wayland deliberately does not expose global window coordinates, so
///          valid outputs receive zero.
void vgfx_platform_get_position(struct vgfx_window *win, int32_t *out_x, int32_t *out_y) {
    (void)win;
    if (out_x)
        *out_x = 0;
    if (out_y)
        *out_y = 0;
}

/// @copydoc vgfx_platform_set_position
/// @details Wayland clients cannot choose global toplevel positions; this is a no-op.
void vgfx_platform_set_position(struct vgfx_window *win, int32_t x, int32_t y) {
    (void)win;
    (void)x;
    (void)y;
}

/// @copydoc vgfx_platform_focus
void vgfx_platform_focus(struct vgfx_window *win) {
    if (win && win->platform_data)
        (void)vgfx_wayland_activation_request(
            &((vgfx_wayland_platform_t *)win->platform_data)->activation);
}

/// @copydoc vgfx_platform_request_foreground
void vgfx_platform_request_foreground(struct vgfx_window *win) {
    vgfx_platform_focus(win);
}

/// @copydoc vgfx_platform_is_focused
int32_t vgfx_platform_is_focused(struct vgfx_window *win) {
    return win && win->platform_data
               ? ((vgfx_wayland_platform_t *)win->platform_data)->shell.activated
               : 0;
}

/// @copydoc vgfx_platform_set_prevent_close
void vgfx_platform_set_prevent_close(struct vgfx_window *win, int32_t prevent) {
    if (win)
        win->prevent_close = prevent ? 1 : 0;
}

/// @copydoc vgfx_platform_set_cursor
void vgfx_platform_set_cursor(struct vgfx_window *win, int32_t cursor_type) {
    if (win && win->platform_data) {
        vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
        platform->cursor_type = cursor_type;
        vgfx_wayland_cursor_set(&platform->cursor, cursor_type, platform->cursor_visible);
    }
}

/// @copydoc vgfx_platform_set_cursor_visible
void vgfx_platform_set_cursor_visible(struct vgfx_window *win, int32_t visible) {
    if (win && win->platform_data) {
        vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
        platform->cursor_visible = visible ? 1 : 0;
        vgfx_wayland_cursor_set(&platform->cursor, platform->cursor_type, platform->cursor_visible);
    }
}

/// @copydoc vgfx_platform_hide_cursor
void vgfx_platform_hide_cursor(void) {
    if (g_vgfx_wayland_active_cursor)
        vgfx_wayland_cursor_set(
            g_vgfx_wayland_active_cursor, g_vgfx_wayland_active_cursor->type, 0);
}

/// @copydoc vgfx_platform_show_cursor
void vgfx_platform_show_cursor(void) {
    if (g_vgfx_wayland_active_cursor)
        vgfx_wayland_cursor_set(
            g_vgfx_wayland_active_cursor, g_vgfx_wayland_active_cursor->type, 1);
}

/// @copydoc vgfx_platform_get_monitor_size
void vgfx_platform_get_monitor_size(struct vgfx_window *win, int32_t *out_w, int32_t *out_h) {
    if (win && win->platform_data) {
        vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
        if (vgfx_wayland_scale_monitor_size(&platform->scale, out_w, out_h))
            return;
    }
    if (out_w)
        *out_w = win ? win->width : VGFX_DEFAULT_WIDTH;
    if (out_h)
        *out_h = win ? win->height : VGFX_DEFAULT_HEIGHT;
}

/// @copydoc vgfx_platform_set_window_size
/// @details Wayland expresses a client resize by committing content at the new
///          logical extent; the next compositor configure remains authoritative.
void vgfx_platform_set_window_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data || w <= 0 || h <= 0)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;

    /* Wayland has no XResizeWindow-style request for an ordinary toplevel.
     * A client-initiated resize is expressed by committing content with the
     * new logical extent. Keep the shell's effective size in sync so a stale
     * compositor configure does not mask the requested size while the new
     * buffer is prepared. The next compositor configure remains authoritative. */
    platform->shell.width = w;
    platform->shell.height = h;
    platform->requested_logical_width = w;
    platform->requested_logical_height = h;
    platform->reported_logical_width = w;
    platform->reported_logical_height = h;
    (void)vgfx_wayland_sync_state(win);
}

/// @copydoc vgfx_platform_set_window_min_size
void vgfx_platform_set_window_min_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data || w <= 0 || h <= 0)
        return;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    if (!platform->shell.toplevel)
        return;
    (void)platform->connection.api.proxy_marshal_flags(
        (struct wl_proxy *)platform->shell.toplevel,
        VGFX_XDG_TOPLEVEL_SET_MIN_SIZE,
        NULL,
        platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.toplevel),
        0,
        w,
        h);
    (void)platform->connection.api.proxy_marshal_flags(
        (struct wl_proxy *)platform->shell.surface,
        VGFX_WL_SURFACE_COMMIT,
        NULL,
        platform->connection.api.proxy_get_version((struct wl_proxy *)platform->shell.surface),
        0);
    (void)platform->connection.api.display_flush(platform->connection.display);
}

/// @copydoc vgfx_clipboard_has_format
int vgfx_clipboard_has_format(vgfx_clipboard_format_t format) {
    return format == VGFX_CLIPBOARD_TEXT && vgfx_wayland_data_has_text(g_vgfx_wayland_active_data);
}

/// @copydoc vgfx_clipboard_get_text
char *vgfx_clipboard_get_text(void) {
    return vgfx_wayland_data_get_text(g_vgfx_wayland_active_data);
}

/// @copydoc vgfx_clipboard_set_text
void vgfx_clipboard_set_text(const char *text) {
    (void)vgfx_wayland_data_set_text(g_vgfx_wayland_active_data, text);
}

/// @copydoc vgfx_clipboard_clear
void vgfx_clipboard_clear(void) {
    vgfx_clipboard_set_text(NULL);
}

/// @copydoc vgfx_get_native_view
void *vgfx_get_native_view(vgfx_window_t window) {
    if (!window || !window->platform_data)
        return NULL;
    return ((vgfx_wayland_platform_t *)window->platform_data)->shell.surface;
}

/// @copydoc vgfx_get_native_display
void *vgfx_get_native_display(vgfx_window_t window) {
    if (!window || !window->platform_data)
        return NULL;
    return ((vgfx_wayland_platform_t *)window->platform_data)->connection.display;
}

/// @copydoc vgfx_get_native_handles
int vgfx_get_native_handles(vgfx_window_t window, vgfx_native_handles_t *out_handles) {
    if (!window || !window->platform_data || !out_handles)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)window->platform_data;
    *out_handles = (vgfx_native_handles_t){.backend = VGFX_NATIVE_BACKEND_WAYLAND,
                                           .display = platform->connection.display,
                                           .surface = platform->shell.surface,
                                           .window = 0};
    return 1;
}

/// @copydoc vgfx_get_window_capabilities
vgfx_window_capabilities_t vgfx_get_window_capabilities(vgfx_window_t window) {
    if (!window || !window->platform_data)
        return 0;
    const vgfx_wayland_platform_t *platform =
        (const vgfx_wayland_platform_t *)window->platform_data;
    const uint32_t globals = platform->connection.globals;
    vgfx_window_capabilities_t result = 0;
    if (platform->data.device)
        result |= VGFX_CAP_CLIPBOARD_TEXT | VGFX_CAP_FILE_DROP;
    if ((globals &
         (VGFX_WAYLAND_GLOBAL_RELATIVE_POINTER_V1 | VGFX_WAYLAND_GLOBAL_POINTER_CONSTRAINTS_V1)) ==
        (VGFX_WAYLAND_GLOBAL_RELATIVE_POINTER_V1 | VGFX_WAYLAND_GLOBAL_POINTER_CONSTRAINTS_V1))
        result |= VGFX_CAP_RELATIVE_MOUSE;
    if (platform->text_input.proxy)
        result |= VGFX_CAP_TEXT_COMPOSITION;
    if (platform->scale.fractional_scale && platform->scale.viewport)
        result |= VGFX_CAP_FRACTIONAL_SCALE;
    if (platform->shell.decoration)
        result |= VGFX_CAP_SERVER_DECORATIONS;
    if (globals & VGFX_WAYLAND_GLOBAL_XDG_ACTIVATION_V1)
        result |= VGFX_CAP_ACTIVATION | VGFX_CAP_FOCUS_REQUEST;
    return result;
}

/// @brief Accept a cursor-warp request as an unsupported Wayland no-op.
/// @param window Ignored target window.
/// @param x Ignored logical X coordinate.
/// @param y Ignored logical Y coordinate.
void vgfx_platform_warp_cursor(vgfx_window_t window, int32_t x, int32_t y) {
    (void)window;
    (void)x;
    (void)y;
}

/// @brief Toggle the Wayland relative-pointer and pointer-constraint session.
/// @param win Window whose relative mode should change.
/// @param enabled Non-zero to enable, zero to disable.
/// @return 1 when the protocol transition succeeded, otherwise 0.
int vgfx_platform_set_relative_mouse(struct vgfx_window *win, int enabled) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    return vgfx_wayland_relative_set_enabled(&platform->relative, enabled);
}

/// @copydoc vgfx_platform_set_text_input_enabled
int vgfx_platform_set_text_input_enabled(struct vgfx_window *win, int32_t enabled) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    return vgfx_wayland_text_input_set_enabled(&platform->text_input, enabled);
}

/// @copydoc vgfx_platform_set_text_input_state
int vgfx_platform_set_text_input_state(struct vgfx_window *win,
                                       const vgfx_text_input_state_t *state) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_wayland_platform_t *platform = (vgfx_wayland_platform_t *)win->platform_data;
    return vgfx_wayland_text_input_set_state(&platform->text_input, state);
}
