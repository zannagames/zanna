//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX Mock Platform Backend (Testing Only)
//
// Provides an in-memory window simulation with NO OS dependencies.  Used for
// unit testing and deterministic behavior verification.  This backend never
// creates real windows or processes OS events - instead, tests manually inject
// events and control time progression.
//
// Key Features:
//   - Deterministic Time: Controllable clock via vgfx_mock_set_time_ms()
//   - Event Injection: Synthetic events via vgfx_mock_inject_*() functions
//   - No External Dependencies: Pure in-memory simulation
//   - No Display: vgfx_platform_present() is a no-op (framebuffer stays in memory)
//
// Use Cases:
//   - Unit Testing: Validate drawing, events, FPS limiting without real windows
//   - CI/CD: Run tests headless on servers without X11/Cocoa/Win32
//   - Determinism: Precise control over time and events for reproducible tests
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Mock platform backend for unit testing ZannaGFX.
/// @details Provides in-memory window simulation with manual event injection
///          and time control.  No real OS windows are created.

#include "vgfx_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Mock Platform State
//===----------------------------------------------------------------------===//

/// @brief Global mock time in milliseconds.
/// @details Controlled by vgfx_mock_set_time_ms() and vgfx_mock_advance_time_ms().
///          Advanced automatically by vgfx_platform_sleep_ms() (simulates sleep).
///          Used by vgfx_platform_now_ms() to return consistent timestamps.
///          Control hooks may deliberately set or advance it to negative values
///          when exercising edge cases.
static int64_t g_mock_time_ms = 0;

/// @brief Mock display scale returned for newly-created windows.
/// @details Defaults to 1.0 so existing tests use physical == logical. Tests
///          can set a HiDPI scale before vgfx_create_window() to validate
///          framebuffer/logical coordinate contracts without a real display.
static float g_mock_display_scale = 1.0f;
static char *g_mock_clipboard_text = NULL;
static vgfx_atomic_flag_t g_mock_clipboard_lock;

/// @brief Acquire the global mock clipboard spin lock.
static void mock_clipboard_lock(void) {
    while (vgfx_atomic_flag_test_and_set(&g_mock_clipboard_lock))
        vgfx_internal_event_wait();
}

/// @brief Release the global mock clipboard spin lock.
static void mock_clipboard_unlock(void) {
    vgfx_atomic_flag_clear(&g_mock_clipboard_lock);
}

#define VGFX_MOCK_PENDING_QUEUE_SLOTS (VGFX_EVENT_QUEUE_SIZE * 8)

/// @brief Mock platform data structure (minimal, no OS resources).
/// @details Stored in vgfx_window->platform_data.  Contains no real handles
///          or resources - just an initialization flag for consistency with
///          other backends.
typedef struct {
    int initialized; ///< 1 if successfully initialized, 0 otherwise
    int focused;     ///< Current mock focus state
    int fullscreen;  ///< Per-window fullscreen state
    int pending_head;
    int pending_tail;
    int wake_pending;
    vgfx_event_t pending_events[VGFX_MOCK_PENDING_QUEUE_SLOTS];
} vgfx_mock_platform;

/// @brief Check whether deterministic mock clipboard text is non-empty.
/// @param format Format to query; only `VGFX_CLIPBOARD_TEXT` is supported.
/// @return 1 for available non-empty text, otherwise 0.
int vgfx_clipboard_has_format(vgfx_clipboard_format_t format) {
    if (format != VGFX_CLIPBOARD_TEXT)
        return 0;
    mock_clipboard_lock();
    int available = g_mock_clipboard_text && g_mock_clipboard_text[0] != '\0';
    mock_clipboard_unlock();
    return available;
}

/// @brief Copy the deterministic mock clipboard text.
/// @return Heap-allocated NUL-terminated text, using an empty string when the
///         clipboard has not been initialized, or NULL on allocation failure.
char *vgfx_clipboard_get_text(void) {
    mock_clipboard_lock();
    const char *source = g_mock_clipboard_text ? g_mock_clipboard_text : "";
    size_t length = strlen(source);
    char *copy = (char *)malloc(length + 1u);
    if (copy)
        memcpy(copy, source, length + 1u);
    mock_clipboard_unlock();
    return copy;
}

/// @brief Replace the deterministic mock clipboard with copied text.
/// @param text Source text, or NULL to publish an empty string.
void vgfx_clipboard_set_text(const char *text) {
    const char *source = text ? text : "";
    size_t length = strlen(source);
    char *copy = (char *)malloc(length + 1u);
    if (!copy)
        return;
    memcpy(copy, source, length + 1u);
    mock_clipboard_lock();
    free(g_mock_clipboard_text);
    g_mock_clipboard_text = copy;
    mock_clipboard_unlock();
}

/// @brief Replace the mock clipboard with an empty string.
void vgfx_clipboard_clear(void) {
    vgfx_clipboard_set_text("");
}

/// @brief Allocate an aligned buffer for the platform-agnostic framebuffer.
/// @details The mock backend cannot rely on OS-specific aligned allocators, so
///          it stores the original `malloc` pointer immediately before the
///          aligned address.  The returned pointer is suitable for
///          `vgfx_platform_aligned_free()` and has at least `alignment` byte
///          alignment when `alignment` is a power of two.
/// @param alignment Required byte alignment; must be a power of two.
/// @param size Number of bytes requested.
/// @return Aligned allocation on success, or NULL for invalid parameters or OOM.
void *vgfx_platform_aligned_alloc(size_t alignment, size_t size) {
    if (size == 0)
        return NULL;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if ((alignment & (alignment - 1u)) != 0)
        return NULL;
    if (size > SIZE_MAX - alignment - sizeof(void *))
        return NULL;

    void *base = malloc(size + alignment - 1u + sizeof(void *));
    if (!base)
        return NULL;
    uintptr_t raw = (uintptr_t)base + sizeof(void *);
    uintptr_t aligned = (raw + (uintptr_t)alignment - 1u) & ~((uintptr_t)alignment - 1u);
    ((void **)aligned)[-1] = base;
    return (void *)aligned;
}

/// @brief Free memory returned by `vgfx_platform_aligned_alloc()`.
/// @details Recovers the hidden base pointer stored by the mock aligned
///          allocator and releases it with `free`.  Passing NULL is a no-op.
/// @param ptr Pointer returned from `vgfx_platform_aligned_alloc()`, or NULL.
void vgfx_platform_aligned_free(void *ptr) {
    if (!ptr)
        return;
    free(((void **)ptr)[-1]);
}

/// @brief Append an injected event to the mock platform's pending queue.
/// @details Uses a fixed ring buffer distinct from the public event queue.
///          When full, the oldest pending injection is discarded so the newest
///          deterministic stimulus is retained.
/// @param platform Mock platform state receiving the event.
/// @param event Event value to copy.
/// @return 1 when copied, or 0 for invalid input.
static int mock_pending_enqueue(vgfx_mock_platform *platform, const vgfx_event_t *event) {
    if (!platform || !event)
        return 0;

    int next_head = (platform->pending_head + 1) % VGFX_MOCK_PENDING_QUEUE_SLOTS;
    if (next_head == platform->pending_tail) {
        platform->pending_tail = (platform->pending_tail + 1) % VGFX_MOCK_PENDING_QUEUE_SLOTS;
    }

    platform->pending_events[platform->pending_head] = *event;
    platform->pending_head = next_head;
    return 1;
}

/// @brief Remove the oldest injected event from the mock pending queue.
/// @param platform Mock platform state to drain.
/// @param out_event Receives the removed event.
/// @return 1 when an event was returned, otherwise 0.
static int mock_pending_dequeue(vgfx_mock_platform *platform, vgfx_event_t *out_event) {
    if (!platform || !out_event || platform->pending_head == platform->pending_tail)
        return 0;

    *out_event = platform->pending_events[platform->pending_tail];
    platform->pending_tail = (platform->pending_tail + 1) % VGFX_MOCK_PENDING_QUEUE_SLOTS;
    return 1;
}

/// @brief Apply one injected event to mock window polling state.
/// @details Mirrors native backend side effects before the event is transferred
///          to the public queue: key/button/mouse state, framebuffer resize,
///          close handling, focus state, and focus-loss input reset.
/// @param win Window receiving state changes.
/// @param platform Mock platform state associated with @p win.
/// @param event Pending event to apply.
static void mock_apply_event(struct vgfx_window *win,
                             vgfx_mock_platform *platform,
                             const vgfx_event_t *event) {
    if (!win || !platform || !event)
        return;

    switch (event->type) {
        case VGFX_EVENT_KEY_DOWN:
            vgfx_internal_set_key_state(win, event->data.key.key, 1);
            break;
        case VGFX_EVENT_KEY_UP:
            vgfx_internal_set_key_state(win, event->data.key.key, 0);
            break;
        case VGFX_EVENT_MOUSE_MOVE:
            vgfx_internal_set_mouse_position(
                win, event->data.mouse_move.x, event->data.mouse_move.y);
            break;
        case VGFX_EVENT_MOUSE_DOWN:
            vgfx_internal_set_mouse_button_state(win, (int32_t)event->data.mouse_button.button, 1);
            vgfx_internal_set_mouse_position(
                win, event->data.mouse_button.x, event->data.mouse_button.y);
            break;
        case VGFX_EVENT_MOUSE_UP:
            vgfx_internal_set_mouse_button_state(win, (int32_t)event->data.mouse_button.button, 0);
            vgfx_internal_set_mouse_position(
                win, event->data.mouse_button.x, event->data.mouse_button.y);
            break;
        case VGFX_EVENT_SCROLL:
            vgfx_internal_set_mouse_position(win, event->data.scroll.x, event->data.scroll.y);
            break;
        case VGFX_EVENT_RESIZE:
            vgfx_internal_resize_framebuffer(
                win, event->data.resize.width, event->data.resize.height);
            break;
        case VGFX_EVENT_CLOSE: {
            vgfx_internal_event_lock(win);
            int prevent_close = win->prevent_close;
            vgfx_internal_event_unlock(win);
            if (!prevent_close)
                vgfx_internal_set_close_requested(win, 1);
            break;
        }
        case VGFX_EVENT_FOCUS_GAINED:
            vgfx_internal_set_focus_state(win, 1);
            platform->focused = 1;
            break;
        case VGFX_EVENT_FOCUS_LOST:
            vgfx_internal_set_focus_state(win, 0);
            platform->focused = 0;
            vgfx_internal_clear_input_state(win);
            break;
        default:
            break;
    }
}

//===----------------------------------------------------------------------===//
// Platform API Implementation (Stubs for Mock)
//===----------------------------------------------------------------------===//

/// @brief Initialize platform-specific window resources (mock version).
/// @details Allocates a minimal platform data structure but creates NO real
///          window.  Always succeeds unless allocation fails.  The framebuffer
///          remains in memory only (no display surface created).
///
/// @param win    Pointer to the ZannaGFX window structure
/// @param params Window creation parameters (unused in mock backend)
/// @return 1 on success, 0 on allocation failure
///
/// @pre  win != NULL
/// @pre  params != NULL
/// @post On success: platform_data allocated, initialized flag set
/// @post On failure: platform_data NULL, error set
///
/// @note This function NEVER creates a real OS window.  It's a testing stub.
int vgfx_platform_init_window(struct vgfx_window *win, const vgfx_window_params_t *params) {
    if (!win || !params)
        return 0;

    /* Allocate mock platform data (minimal structure) */
    vgfx_mock_platform *platform = (vgfx_mock_platform *)calloc(1, sizeof(vgfx_mock_platform));
    if (!platform) {
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate mock platform data");
        return 0;
    }

    platform->initialized = 1;
    platform->focused = 1;
    win->platform_data = platform;
    vgfx_internal_set_focus_state(win, 1);

    /* Success - no actual window created */
    return 1;
}

/// @brief Destroy platform-specific window resources (mock version).
/// @details Frees the platform data structure.  No real window to close.
///          Safe to call even if init failed.
///
/// @param win Pointer to the ZannaGFX window structure
///
/// @pre  win != NULL
/// @post platform_data freed and set to NULL
void vgfx_platform_destroy_window(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;

    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    free(platform);
    win->platform_data = NULL;
}

/// @brief Wait for a pending mock event or advance deterministic time.
/// @details Returns immediately when an injected event is pending.  Otherwise
///          a positive timeout is simulated by advancing the mock clock without
///          blocking, after which the queue is checked again.
/// @param win Mock window whose pending queue should be inspected.
/// @param timeout_ms Maximum simulated wait in milliseconds.
/// @return 1 when an event is pending, otherwise 0.
int vgfx_platform_wait_events(struct vgfx_window *win, int32_t timeout_ms) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    vgfx_internal_event_lock(win);
    int was_woken = platform->wake_pending;
    platform->wake_pending = 0;
    vgfx_internal_event_unlock(win);
    if (was_woken)
        return 1;
    /* Events already pending: return immediately. */
    if (platform->pending_head != platform->pending_tail)
        return 1;
    if (timeout_ms > 0)
        vgfx_platform_sleep_ms(timeout_ms);
    /* An event may have been injected concurrently while we slept. */
    vgfx_internal_event_lock(win);
    was_woken = platform->wake_pending;
    platform->wake_pending = 0;
    vgfx_internal_event_unlock(win);
    return was_woken || platform->pending_head != platform->pending_tail ? 1 : 0;
}

/// @copydoc vgfx_platform_wake_events
int vgfx_platform_wake_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    vgfx_internal_event_lock(win);
    platform->wake_pending = 1;
    vgfx_internal_event_unlock(win);
    return 1;
}

/// @brief Transfer injected mock events into core window state and queue.
/// @details Drains the platform-side pending queue in order, applies the same
///          sticky state transitions a native backend would perform, and then
///          enqueues each event through the synchronized core queue.
/// @param win Mock window whose injected events should be processed.
/// @return 1 after draining valid platform state, otherwise 0.
int vgfx_platform_process_events(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;

    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    vgfx_event_t event;
    while (mock_pending_dequeue(platform, &event)) {
        mock_apply_event(win, platform, &event);
        vgfx_internal_enqueue_event(win, &event);
    }

    return 1;
}

/// @brief Present framebuffer to window (mock version - no-op).
/// @details The mock backend has no display surface.  The framebuffer remains
///          in memory and can be inspected directly via win->pixels.  This
///          function always succeeds and does nothing.
///
/// @param win Pointer to the ZannaGFX window structure (unused)
/// @return 1 (always succeeds)
///
/// @note To verify rendering, tests can directly read win->pixels.
int vgfx_platform_present(struct vgfx_window *win) {
    (void)win;
    /* Mock backend has no display to update */
    /* Framebuffer remains in memory for inspection by tests */
    return 1;
}

/// @brief Sleep for the specified duration (mock version - advances time).
/// @details Advances the global mock time by `ms` milliseconds.  Does NOT
///          actually sleep (no blocking).  Used by vgfx_update() for FPS
///          limiting in tests.
///
/// @param ms Duration to "sleep" in milliseconds
///
/// @post g_mock_time_ms increased by ms (if ms > 0)
///
/// @note This allows deterministic testing of FPS limiting without waiting.
void vgfx_platform_sleep_ms(int32_t ms) {
    if (ms > 0) {
        g_mock_time_ms += ms;
    }
}

/// @brief Yield during mock event-lock contention.
/// @details The mock backend runs deterministically on test-controlled time, so
///          this scheduler yield is intentionally a no-op and does not advance
///          g_mock_time_ms.
void vgfx_platform_yield(void) {}

/// @brief Query the configurable mock HiDPI scale factor.
/// @details Defaults to 1.0 for physical/logical parity.  Tests may change it
///          with `vgfx_mock_set_display_scale()` to exercise scaled coordinate
///          and framebuffer contracts without a real display.
/// @return Sanitized mock physical-pixels-per-logical-unit scale.
float vgfx_platform_get_display_scale(void) {
    return vgfx_internal_sanitize_scale(g_mock_display_scale);
}

/// @brief Get current time in milliseconds (mock version - returns mock time).
/// @details Returns the global mock time, which is controlled by test code.
///          The epoch is arbitrary (typically starts at 0 when tests begin).
///
/// @return Current mock time in milliseconds
///
/// @note Time progression is entirely manual in the mock backend.  Call
///       vgfx_mock_advance_time_ms() or vgfx_platform_sleep_ms() to advance time.
int64_t vgfx_platform_now_ms(void) {
    return g_mock_time_ms;
}

//===----------------------------------------------------------------------===//
// Mock Control Functions (Test API)
//===----------------------------------------------------------------------===//
// These functions are NOT part of the platform abstraction layer.  They are
// test utilities for controlling the mock backend's time simulation.
//===----------------------------------------------------------------------===//

/// @brief Set the mock time to an absolute value.
/// @details Directly sets g_mock_time_ms to the specified value.  Useful for
///          resetting time between tests or simulating specific timestamps.
///
/// @param ms New mock time in milliseconds
///
/// @post g_mock_time_ms == ms
/// @post vgfx_platform_now_ms() returns ms
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_set_time_ms(int64_t ms) {
    g_mock_time_ms = ms;
}

/// @brief Return the mock backend's absent native display handle.
/// @param window Ignored mock window.
/// @return Always NULL.
void *vgfx_get_native_display(vgfx_window_t window) {
    (void)window;
    return NULL;
}

/// @brief Return the mock backend's absent native view handle.
/// @param window Ignored mock window.
/// @return Always NULL.
void *vgfx_get_native_view(vgfx_window_t window) {
    (void)window;
    return NULL; /* Mock backend has no native view */
}

/// @brief Fill an explicit no-native-backend handle descriptor.
/// @param window Mock window used only for validity checking.
/// @param out_handles Receives a zeroed descriptor with backend `NONE`.
/// @return 1 when the descriptor was written, otherwise 0.
int vgfx_get_native_handles(vgfx_window_t window, vgfx_native_handles_t *out_handles) {
    if (!window || !out_handles)
        return 0;
    *out_handles = (vgfx_native_handles_t){.backend = VGFX_NATIVE_BACKEND_NONE};
    return 1;
}

/// @brief Report that the mock backend exposes no native window capabilities.
/// @param window Ignored mock window.
/// @return Always zero.
vgfx_window_capabilities_t vgfx_get_window_capabilities(vgfx_window_t window) {
    (void)window;
    return 0;
}

/// @brief Accept a cursor-warp request without native side effects.
/// @param win Ignored mock window.
/// @param x Ignored logical X coordinate.
/// @param y Ignored logical Y coordinate.
void vgfx_platform_warp_cursor(struct vgfx_window *win, int32_t x, int32_t y) {
    (void)win;
    (void)x;
    (void)y;
}

/// @brief Accept a process cursor-hide request as a deterministic no-op.
void vgfx_platform_hide_cursor(void) {}

/// @brief Accept a process cursor-show request as a deterministic no-op.
void vgfx_platform_show_cursor(void) {}

/// @brief Mock display size: unavailable, so fullscreen creation falls back
///        to the default window dimensions (deterministic for tests).
/// @param out_w Ignored width output.
/// @param out_h Ignored height output.
/// @return Always 0 to report unavailable display dimensions.
int vgfx_platform_get_display_logical_size(int32_t *out_w, int32_t *out_h) {
    (void)out_w;
    (void)out_h;
    return 0;
}

/// @brief Mock relative mouse mode: always reports native raw support.
/// @details Tests drive the accumulators deterministically through
///          vgfx_mock_push_relative_delta(), exercising the exact code path
///          real backends use (vgfx_internal_add_relative_delta +
///          vgfx_get_relative_deltas drain).
/// @param win Ignored mock window.
/// @param enabled Ignored requested state.
/// @return Always 1 to expose the native-relative test path.
int vgfx_platform_set_relative_mouse(struct vgfx_window *win, int enabled) {
    (void)win;
    (void)enabled;
    return 1;
}

/// @brief Accept mock text-input enablement for a valid window.
/// @param win Window whose presence is validated.
/// @param enabled Ignored requested state.
/// @return 1 for a non-NULL window, otherwise 0.
int vgfx_platform_set_text_input_enabled(struct vgfx_window *win, int32_t enabled) {
    (void)enabled;
    return win != NULL;
}

/// @brief Accept a mock surrounding-text snapshot for a valid window.
/// @param win Window whose presence is validated.
/// @param state Text-input state whose presence is validated.
/// @return 1 when both pointers are non-NULL, otherwise 0.
int vgfx_platform_set_text_input_state(struct vgfx_window *win,
                                       const vgfx_text_input_state_t *state) {
    return win && state;
}

/// @brief Inject a synthetic relative mouse motion delta (test hook).
/// @details Accumulates into the window's relative delta accumulators exactly
///          like a platform raw-motion event would. Only meaningful while
///          relative mouse mode is enabled on @p window.
/// @param window Target window.
/// @param dx Horizontal motion delta (sub-pixel capable).
/// @param dy Vertical motion delta, positive = down (sub-pixel capable).
void vgfx_mock_push_relative_delta(vgfx_window_t window, double dx, double dy) {
    if (!window)
        return;
    vgfx_internal_add_relative_delta(window, dx, dy);
}

/// @brief Get the current mock time.
/// @details Returns the current value of g_mock_time_ms.  Equivalent to
///          vgfx_platform_now_ms() but more explicit for test code.
///
/// @return Current mock time in milliseconds
///
/// @note Only available in mock backend (not in real platform backends).
int64_t vgfx_mock_get_time_ms(void) {
    return g_mock_time_ms;
}

/// @brief Advance mock time by a relative delta.
/// @details Increments g_mock_time_ms by the specified delta.  Useful for
///          simulating time progression in tests without setting absolute
///          timestamps.
///
/// @param delta_ms Amount to advance time in milliseconds (can be negative)
///
/// @post g_mock_time_ms increased by delta_ms
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_advance_time_ms(int64_t delta_ms) {
    g_mock_time_ms += delta_ms;
}

/// @brief Set the display scale returned by the mock backend.
/// @details Sanitizes the requested value using the production scale bounds,
///          allowing deterministic HiDPI framebuffer/coordinate tests.
/// @param scale Requested physical-pixels-per-logical-unit scale.
void vgfx_mock_set_display_scale(float scale) {
    g_mock_display_scale = vgfx_internal_sanitize_scale(scale);
}

//===----------------------------------------------------------------------===//
// Event Injection Functions (Test API)
//===----------------------------------------------------------------------===//
// These functions allow tests to synthetically generate events as if they
// came from the OS.  Events are enqueued using the same mechanism as real
// platform backends, so the core library processes them identically.
//===----------------------------------------------------------------------===//

/// @brief Inject a synthetic keyboard event.
/// @details Appends a KEY_DOWN or KEY_UP event to the platform-side pending
///          queue with the current mock timestamp.  The next
///          `vgfx_platform_process_events()` call applies key polling state and
///          transfers the event to the public queue.
///
/// @param window Window handle
/// @param key    Key code (must be < 512 and != VGFX_KEY_UNKNOWN)
/// @param down   1 for key press, 0 for key release
///
/// @pre  window != NULL
/// @pre  key < 512 && key != VGFX_KEY_UNKNOWN
/// @post A corresponding KEY_DOWN or KEY_UP event is pending for processing.
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_inject_key_event(vgfx_window_t window, vgfx_key_t key, int down) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win || key == VGFX_KEY_UNKNOWN || key >= 512)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;

    /* Enqueue event with current mock time */
    vgfx_event_t event = {.type = down ? VGFX_EVENT_KEY_DOWN : VGFX_EVENT_KEY_UP,
                          .time_ms = g_mock_time_ms,
                          .data.key = {.key = key,
                                       .is_repeat = 0, /* Mock backend never generates repeats */
                                       .modifiers = 0}};
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject a synthetic mouse move event.
/// @details Simulates mouse cursor movement.  Updates win->mouse_x and
///          win->mouse_y, then enqueues a MOUSE_MOVE event.  Coordinates
///          can be out of bounds (test code may want to simulate that).
///
/// @param window Window handle
/// @param x      New mouse X coordinate
/// @param y      New mouse Y coordinate
///
/// @pre  window != NULL
/// @post win->mouse_x == x
/// @post win->mouse_y == y
/// @post MOUSE_MOVE event enqueued
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_inject_mouse_move(vgfx_window_t window, int32_t x, int32_t y) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;
    vgfx_internal_set_mouse_position(win, x, y);

    /* Enqueue event with current mock time */
    vgfx_event_t event = {.type = VGFX_EVENT_MOUSE_MOVE,
                          .time_ms = g_mock_time_ms,
                          .data.mouse_move = {.x = x, .y = y, .modifiers = 0}};
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject a synthetic mouse button event.
/// @details Appends a MOUSE_DOWN or MOUSE_UP event carrying the current mouse
///          position.  The next platform event pump applies button state and
///          transfers it to the public queue.
///
/// @param window Window handle
/// @param btn    Button code (must be < 8)
/// @param down   1 for button press, 0 for button release
///
/// @pre  window != NULL
/// @pre  btn < 8
/// @post A corresponding MOUSE_DOWN or MOUSE_UP event is pending for processing.
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_inject_mouse_button(vgfx_window_t window, vgfx_mouse_button_t btn, int down) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win || (int)btn < 0 || btn >= 8)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;

    /* Enqueue event with current mock time and mouse position */
    int32_t x = 0;
    int32_t y = 0;
    vgfx_internal_event_lock(win);
    x = win->mouse_x;
    y = win->mouse_y;
    vgfx_internal_event_unlock(win);
    vgfx_event_t event = {.type = down ? VGFX_EVENT_MOUSE_DOWN : VGFX_EVENT_MOUSE_UP,
                          .time_ms = g_mock_time_ms,
                          .data.mouse_button = {.x = x, .y = y, .button = btn, .modifiers = 0}};
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject a synthetic resize event.
/// @details Appends a RESIZE event containing physical dimensions and derived
///          logical dimensions.  The next platform event pump reallocates the
///          framebuffer and transfers the event to the public queue.
///
/// @param window Window handle
/// @param width  New window width in pixels
/// @param height New window height in pixels
///
/// @pre  window != NULL
/// @post A RESIZE event is pending for processing.
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_inject_resize(vgfx_window_t window, int32_t width, int32_t height) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;

    /* Enqueue resize event with current mock time */
    vgfx_event_t event = {0};
    vgfx_internal_init_resize_event(&event, win, g_mock_time_ms, width, height);
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject a synthetic close event.
/// @details Simulates the user closing the window (clicking the X button).
///          Appends a CLOSE event for the next platform pump.  Processing sets
///          close-requested state unless prevention is active but never destroys
///          the window; test code must call vgfx_destroy_window() explicitly.
///
/// @param window Window handle
///
/// @pre  window != NULL
/// @post A CLOSE event is pending for processing.
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_inject_close(vgfx_window_t window) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;

    /* Enqueue close event with current mock time */
    vgfx_event_t event = {.type = VGFX_EVENT_CLOSE, .time_ms = g_mock_time_ms};
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject a synthetic focus event.
/// @details Simulates the window gaining or losing focus (becoming active or
///          inactive).  The next platform pump applies focus/input state and
///          publishes FOCUS_GAINED or FOCUS_LOST.
///
/// @param window Window handle
/// @param gained 1 for focus gained, 0 for focus lost
///
/// @pre  window != NULL
/// @post A corresponding focus event is pending for processing.
///
/// @note Only available in mock backend (not in real platform backends).
void vgfx_mock_inject_focus(vgfx_window_t window, int gained) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;

    /* Enqueue focus event with current mock time */
    vgfx_event_t event = {.type = gained ? VGFX_EVENT_FOCUS_GAINED : VGFX_EVENT_FOCUS_LOST,
                          .time_ms = g_mock_time_ms};
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject one filtered Unicode text-input event.
/// @details Applies the production text-scalar policy with no modifiers, then
///          appends an accepted event using the current mock timestamp.
/// @param window Mock window receiving the event.
/// @param codepoint Candidate Unicode scalar.
void vgfx_mock_inject_text_input(vgfx_window_t window, uint32_t codepoint) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;
    if (!vgfx_internal_should_emit_text_input(codepoint, 0))
        return;

    vgfx_event_t event = {.type = VGFX_EVENT_TEXT_INPUT,
                          .time_ms = g_mock_time_ms,
                          .data.text = {.codepoint = codepoint, .modifiers = 0}};
    mock_pending_enqueue(platform, &event);
}

/// @brief Build and enqueue one mock composition lifecycle event.
/// @details This shared helper applies the production inline UTF-8 bound and metadata rules before
///          placing the value in the deterministic pending queue.
/// @param window Mock window receiving the event.
/// @param type Composition lifecycle discriminator.
/// @param text Borrowed NUL-terminated UTF-8 text; NULL becomes empty.
/// @param selection_start Preedit selection start in codepoints.
/// @param selection_length Preedit selected codepoint count.
/// @param replacement_start Committed codepoint start, or -1 for current selection.
/// @param replacement_length Committed codepoint count, or -1 with sentinel start.
static void mock_inject_composition(vgfx_window_t window,
                                    vgfx_event_type_t type,
                                    const char *text,
                                    int32_t selection_start,
                                    int32_t selection_length,
                                    int32_t replacement_start,
                                    int32_t replacement_length) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;
    const char *safe_text = text ? text : "";
    vgfx_event_t event;
    if (!vgfx_internal_init_composition_event(&event,
                                              type,
                                              g_mock_time_ms,
                                              safe_text,
                                              strlen(safe_text),
                                              selection_start,
                                              selection_length,
                                              replacement_start,
                                              replacement_length,
                                              0))
        return;
    mock_pending_enqueue(platform, &event);
}

/// @brief Inject the beginning of a deterministic native IME session.
/// @param window Mock window receiving the event.
/// @param replacement_start Committed-text codepoint boundary, or -1 for current selection.
/// @param replacement_length Committed-text codepoint count, or -1 with sentinel start.
void vgfx_mock_inject_composition_start(vgfx_window_t window,
                                        int32_t replacement_start,
                                        int32_t replacement_length) {
    mock_inject_composition(
        window, VGFX_EVENT_COMPOSITION_START, "", 0, 0, replacement_start, replacement_length);
}

/// @brief Inject a native IME preedit text and internal selection update.
/// @param window Mock window receiving the event.
/// @param text Borrowed NUL-terminated UTF-8 preedit; NULL becomes empty.
/// @param selection_start Preedit caret/selection start in Unicode codepoints.
/// @param selection_length Selected preedit codepoint count.
void vgfx_mock_inject_composition_update(vgfx_window_t window,
                                         const char *text,
                                         int32_t selection_start,
                                         int32_t selection_length) {
    mock_inject_composition(
        window, VGFX_EVENT_COMPOSITION_UPDATE, text, selection_start, selection_length, -1, -1);
}

/// @brief Inject one atomic native IME commit value.
/// @param window Mock window receiving the event.
/// @param text Borrowed NUL-terminated UTF-8 commit; NULL becomes empty.
void vgfx_mock_inject_composition_commit(vgfx_window_t window, const char *text) {
    mock_inject_composition(window, VGFX_EVENT_COMPOSITION_COMMIT, text, 0, 0, -1, -1);
}

/// @brief Inject cancellation of the active native IME session.
/// @param window Mock window receiving the event.
void vgfx_mock_inject_composition_cancel(vgfx_window_t window) {
    mock_inject_composition(window, VGFX_EVENT_COMPOSITION_CANCEL, "", 0, 0, -1, -1);
}

/// @brief Inject a synthetic scroll-wheel event.
/// @param window Mock window receiving the event.
/// @param dx Horizontal scroll delta.
/// @param dy Vertical scroll delta.
/// @param x Logical pointer X coordinate accompanying the event.
/// @param y Logical pointer Y coordinate accompanying the event.
void vgfx_mock_inject_scroll(vgfx_window_t window, float dx, float dy, int32_t x, int32_t y) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    if (!platform)
        return;

    vgfx_event_t event = {
        .type = VGFX_EVENT_SCROLL,
        .time_ms = g_mock_time_ms,
        .data.scroll = {.delta_x = dx, .delta_y = dy, .x = x, .y = y, .modifiers = 0}};
    mock_pending_enqueue(platform, &event);
}

//===----------------------------------------------------------------------===//
// Window Title and Fullscreen (Mock)
//===----------------------------------------------------------------------===//

/// @brief Set the window title (mock version - no-op).
/// @details The mock backend has no title bar, so this is a no-op.
///
/// @param win   Pointer to the window structure (unused)
/// @param title New title string (unused)
void vgfx_platform_set_title(struct vgfx_window *win, const char *title) {
    (void)win;
    (void)title;
    /* Mock backend has no title bar - no-op */
}

/// @brief Set fullscreen mode (mock version - updates state only).
/// @details The mock backend has no display, but tracks the fullscreen state
///          for test verification.
///
/// @param win        Pointer to the window structure
/// @param fullscreen 1 for fullscreen, 0 for windowed
/// @return 1 (always succeeds)
int vgfx_platform_set_fullscreen(struct vgfx_window *win, int fullscreen) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    platform->fullscreen = fullscreen ? 1 : 0;
    return 1;
}

/// @brief Check fullscreen mode (mock version).
/// @param win Pointer to the window structure (unused)
/// @return Current mock fullscreen state
int vgfx_platform_is_fullscreen(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    return platform->fullscreen ? 1 : 0;
}

//===----------------------------------------------------------------------===//
// Window Management Stubs (added with platform extensions)
//===----------------------------------------------------------------------===//

/// @brief Accept a minimize request without changing mock state.
/// @param win Ignored mock window.
void vgfx_platform_minimize(struct vgfx_window *win) {
    (void)win;
}

/// @brief Accept a maximize request without changing mock state.
/// @param win Ignored mock window.
void vgfx_platform_maximize(struct vgfx_window *win) {
    (void)win;
}

/// @brief Accept a restore request without changing mock state.
/// @param win Ignored mock window.
void vgfx_platform_restore(struct vgfx_window *win) {
    (void)win;
}

/// @brief Report that mock windows are never minimized.
/// @param win Ignored mock window.
/// @return Always 0.
int32_t vgfx_platform_is_minimized(struct vgfx_window *win) {
    (void)win;
    return 0;
}

/// @brief Report that mock windows are never maximized.
/// @param win Ignored mock window.
/// @return Always 0.
int32_t vgfx_platform_is_maximized(struct vgfx_window *win) {
    (void)win;
    return 0;
}

/// @brief Return the deterministic mock window origin.
/// @param win Ignored mock window.
/// @param x Receives zero when non-NULL.
/// @param y Receives zero when non-NULL.
void vgfx_platform_get_position(struct vgfx_window *win, int32_t *x, int32_t *y) {
    (void)win;
    if (x)
        *x = 0;
    if (y)
        *y = 0;
}

/// @brief Accept a mock window-position request as a no-op.
/// @param win Ignored mock window.
/// @param x Ignored X coordinate.
/// @param y Ignored Y coordinate.
void vgfx_platform_set_position(struct vgfx_window *win, int32_t x, int32_t y) {
    (void)win;
    (void)x;
    (void)y;
}

/// @brief Give a mock window focus through the foreground-request path.
/// @param win Window whose focus state should be set.
void vgfx_platform_focus(struct vgfx_window *win) {
    vgfx_platform_request_foreground(win);
}

/// @brief Mark a valid mock window focused.
/// @details Updates both backend-local and core synchronized focus state.
/// @param win Window to focus.
void vgfx_platform_request_foreground(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    platform->focused = 1;
    vgfx_internal_set_focus_state(win, 1);
}

/// @brief Query mock backend focus state.
/// @param win Window whose focus flag should be read.
/// @return 1 when focused, otherwise 0.
int32_t vgfx_platform_is_focused(struct vgfx_window *win) {
    if (!win || !win->platform_data)
        return 0;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    return platform->focused ? 1 : 0;
}

/// @brief Update the core close-prevention flag for a mock window.
/// @param win Window whose close policy should change.
/// @param p Non-zero to prevent close-request state, zero to allow it.
void vgfx_platform_set_prevent_close(struct vgfx_window *win, int32_t p) {
    vgfx_internal_set_prevent_close(win, p);
}

/// @brief Accept a cursor-shape request without native side effects.
/// @param win Ignored mock window.
/// @param type Ignored public cursor type.
void vgfx_platform_set_cursor(struct vgfx_window *win, int32_t type) {
    (void)win;
    (void)type;
}

/// @brief Accept a cursor-visibility request without native side effects.
/// @param win Ignored mock window.
/// @param visible Ignored visibility state.
void vgfx_platform_set_cursor_visible(struct vgfx_window *win, int32_t visible) {
    (void)win;
    (void)visible;
}

/// @brief Return deterministic physical monitor dimensions for mock tests.
/// @param win Ignored mock window.
/// @param out_w Receives 1920 when non-NULL.
/// @param out_h Receives 1080 when non-NULL.
void vgfx_platform_get_monitor_size(struct vgfx_window *win, int32_t *out_w, int32_t *out_h) {
    (void)win;
    if (out_w)
        *out_w = 1920;
    if (out_h)
        *out_h = 1080;
}

/// @brief Queue a mock resize request for the next event pump.
/// @details Creates a resize event with the current mock timestamp and active
///          coordinate scale; framebuffer reallocation occurs during processing.
/// @param win Window to resize.
/// @param w Requested physical framebuffer width used by this mock hook.
/// @param h Requested physical framebuffer height used by this mock hook.
void vgfx_platform_set_window_size(struct vgfx_window *win, int32_t w, int32_t h) {
    if (!win || !win->platform_data)
        return;
    vgfx_mock_platform *platform = (vgfx_mock_platform *)win->platform_data;
    vgfx_event_t event = {0};
    vgfx_internal_init_resize_event(&event, win, g_mock_time_ms, w, h);
    mock_pending_enqueue(platform, &event);
}

/// @brief Accept minimum-size publication without storing native constraints.
/// @param win Ignored mock window.
/// @param w Ignored minimum width.
/// @param h Ignored minimum height.
void vgfx_platform_set_window_min_size(struct vgfx_window *win, int32_t w, int32_t h) {
    (void)win;
    (void)w;
    (void)h;
}

//===----------------------------------------------------------------------===//
// End of Mock Backend
//===----------------------------------------------------------------------===//
