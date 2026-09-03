//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX Internal Structures and Platform Abstraction
//
// Defines the internal window representation, platform backend interface,
// and internal helper functions.  This header is NOT part of the public API
// and is only included by ZannaGFX implementation files.
//
// Platform Backend Contract:
//   Each platform backend (vgfx_platform_*.c) must implement the platform
//   abstraction functions defined below.  The backend is responsible for:
//     - Creating/destroying native OS windows
//     - Processing OS events and translating them to vgfx_event_t
//     - Presenting (blitting) the framebuffer to the screen
//     - Providing high-resolution timing and sleep functions
//
// Internal Window Structure:
//   The vgfx_window structure is the complete representation of a window,
//   containing the framebuffer, event queue, input state, timing info, and
//   platform-specific data.  The public API only exposes an opaque handle.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Internal structures and platform abstraction layer for ZannaGFX.
/// @details Not part of the public API.  Defines the complete window structure,
///          platform backend interface, and internal helper functions.

#pragma once

#include "vgfx.h"
#include "vgfx_config.h"
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <stdatomic.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
/// @brief MSVC-compatible storage for the internal event spin-lock flag.
/// @details Access only through the `vgfx_atomic_flag_*` wrappers, which map
///          the long-compatible value to interlocked exchange operations.
typedef volatile long vgfx_atomic_flag_t;

/// @brief Initialize a ZannaGFX atomic flag to the unlocked state.
/// @details MSVC's Interlocked-backed flag is a plain long-compatible value, so
///          writing zero before publication is sufficient and avoids depending
///          on calloc'd storage for synchronization state.
/// @param flag Atomic flag to initialize. NULL is ignored.
static inline void vgfx_atomic_flag_init(vgfx_atomic_flag_t *flag) {
    if (flag)
        *flag = 0;
}

/// @brief Atomically acquire a ZannaGFX flag and report its previous state.
/// @details Replaces the flag with the locked value using an acquire-capable
///          MSVC interlocked exchange.  The event-queue spin lock repeatedly
///          calls this helper until the previous state was clear.
/// @param flag Initialized flag to acquire; must not be NULL.
/// @return Non-zero when the flag was already set, or zero when this call
///         acquired a previously clear flag.
static inline int vgfx_atomic_flag_test_and_set(vgfx_atomic_flag_t *flag) {
    return _InterlockedExchange(flag, 1) != 0;
}

/// @brief Release a ZannaGFX atomic flag.
/// @details Publishes the unlocked value with the MSVC interlocked primitive
///          paired with `vgfx_atomic_flag_test_and_set()`.
/// @param flag Acquired flag to release; must not be NULL.
static inline void vgfx_atomic_flag_clear(vgfx_atomic_flag_t *flag) {
    (void)_InterlockedExchange(flag, 0);
}
#else
/// @brief C11 atomic storage for the internal event spin-lock flag.
/// @details Uses an atomic Boolean with explicit acquire/release operations so
///          dynamically allocated windows can initialize it at runtime.
typedef atomic_bool vgfx_atomic_flag_t;

/// @brief Initialize a ZannaGFX atomic flag to the unlocked state.
/// @details The internal non-MSVC flag is an atomic boolean instead of C11's
///          `atomic_flag` so heap-allocated window objects have a well-defined
///          runtime initialization path.
/// @param flag Atomic flag to initialize. NULL is ignored.
static inline void vgfx_atomic_flag_init(vgfx_atomic_flag_t *flag) {
    if (flag)
        atomic_init(flag, false);
}

/// @brief Atomically acquire a ZannaGFX flag and report its previous state.
/// @details Performs an acquire exchange so subsequent accesses protected by
///          the event lock observe writes published by its previous owner.
/// @param flag Initialized flag to acquire; must not be NULL.
/// @return Non-zero when the flag was already set, or zero when this call
///         acquired a previously clear flag.
static inline int vgfx_atomic_flag_test_and_set(vgfx_atomic_flag_t *flag) {
    return atomic_exchange_explicit(flag, true, memory_order_acquire);
}

/// @brief Release a ZannaGFX atomic flag.
/// @details Uses release ordering so queue and input-state writes made inside
///          the critical section become visible to the next acquiring thread.
/// @param flag Acquired flag to release; must not be NULL.
static inline void vgfx_atomic_flag_clear(vgfx_atomic_flag_t *flag) {
    atomic_store_explicit(flag, false, memory_order_release);
}
#endif

//===----------------------------------------------------------------------===//
// Internal Constants
//===----------------------------------------------------------------------===//

/// @def VGFX_INTERNAL_EVENT_QUEUE_SLOTS
/// @brief Physical array size for the synchronized ring buffer.
/// @details One extra slot is allocated beyond the advertised capacity to
///          distinguish between full and empty states without using separate
///          counters.  When (head + 1) % SLOTS == tail, the queue is full.
///          When head == tail, the queue is empty.
#define VGFX_INTERNAL_EVENT_QUEUE_SLOTS (VGFX_EVENT_QUEUE_SIZE + 1)

/// @def VGFX_CLIP_LIMIT_MAX_DEPTH
/// @brief Maximum nesting depth for clip-limit scopes.
/// @details The retained compositor pushes at most one damage limit per frame
///          and each nested internally-clipping container (scroll views,
///          floating panels) adds one scope, so real trees stay far below
///          this bound. Pushes beyond the bound fail gracefully (return 0)
///          and callers fall back to plain clip rectangles.
#define VGFX_CLIP_LIMIT_MAX_DEPTH 16

//===----------------------------------------------------------------------===//
// Internal Window Structure
//===----------------------------------------------------------------------===//

/// @brief Retired framebuffer allocation kept alive after a resize.
/// @details Direct framebuffer descriptors returned by `vgfx_get_framebuffer`
///          cannot be pinned by the current public API.  When a resize replaces
///          `vgfx_window::pixels`, old allocations are linked here and released
///          during window destruction so stale descriptors are not immediately
///          turned into dangling pointers.  Callers should still compare
///          generations and reacquire the current framebuffer after resize.
typedef struct vgfx_retired_framebuffer {
    uint8_t *pixels;                       ///< Previous aligned RGBA pixel buffer.
    struct vgfx_retired_framebuffer *next; ///< Next retired allocation, or NULL.
} vgfx_retired_framebuffer_t;

/// @brief Complete internal representation of a ZannaGFX window.
/// @details Contains all state required to manage a window: framebuffer,
///          event queue, input tracking, timing, and platform-specific data.
///          The public API exposes this as an opaque vgfx_window_t handle.
///
/// @invariant width > 0 && height > 0
/// @invariant pixels != NULL (4-byte RGBA framebuffer)
/// @invariant stride == width * 4
/// @invariant 0 <= event_head < VGFX_INTERNAL_EVENT_QUEUE_SLOTS
/// @invariant 0 <= event_tail < VGFX_INTERNAL_EVENT_QUEUE_SLOTS
/// @invariant event_overflow >= 0
/// @invariant mouse_x, mouse_y reflect last known cursor position
/// @invariant key_state[k] is 1 if key k is pressed, 0 if released
/// @invariant platform_data is allocated/owned by the platform backend
struct vgfx_window {
    //===------------------------------------------------------------------===//
    // Window Properties
    //===------------------------------------------------------------------===//

    /// @brief Current framebuffer width in physical pixels.
    int32_t width;

    /// @brief Current framebuffer height in physical pixels.
    int32_t height;

    /// @brief HiDPI backing scale factor (e.g. 2.0 on Retina, 1.0 elsewhere).
    /// @details Populated by vgfx_create_window via vgfx_platform_get_display_scale().
    ///          win->width and win->height store PHYSICAL pixels after initialisation;
    ///          divide by scale_factor to recover the logical (point) dimensions.
    float scale_factor;

    /// @brief Coordinate transform scale (default 1.0 = no transform).
    /// @details When > 1.0, all public drawing API coordinates are multiplied by
    ///          this value before operating on the physical-pixel framebuffer.
    ///          Mouse positions are divided by this value before returning.
    ///          Set via vgfx_set_coord_scale() — the Canvas API enables this so
    ///          games/demos draw in logical pixels; the GUI layer leaves it at 1.0.
    float coord_scale;

    /// @brief Whether a caller selected the coordinate scale explicitly.
    /// @details Distinguishes the Canvas layer opting into logical coordinates
    ///          from the GUI layer's untouched 1.0 default. The two are
    ///          otherwise indistinguishable whenever the backing scale is also
    ///          1.0, which is the normal state on backends that report HiDPI
    ///          asynchronously after the window exists (Wayland). Only an
    ///          explicit selection follows later backing-scale changes.
    int coord_scale_explicit;

    /// @brief Target frame rate for this window.
    /// @details fps > 0: Target that specific FPS with frame limiting.
    ///          fps < 0: Unlimited (no frame rate limiting).
    ///          fps == 0: Should not occur after vgfx_create_window().
    int32_t fps;

    /// @brief Whether the window is resizable (1 = yes, 0 = no).
    /// @details Backends use this to decide whether user-initiated resizing is
    ///          allowed at the native window level.
    int32_t resizable;

    /// @brief Minimum logical client/content width accepted by resize requests.
    /// @details Defaults to one. Native adapters publish this floor to their
    ///          window manager while the core also clamps programmatic resizes.
    int32_t min_width;

    /// @brief Minimum logical client/content height accepted by resize requests.
    /// @copydetails min_width
    int32_t min_height;

    //===------------------------------------------------------------------===//
    // Framebuffer
    //===------------------------------------------------------------------===//

    /// @brief Pointer to RGBA pixel data (width × height × 4 bytes).
    /// @details Memory is aligned to VGFX_FRAMEBUFFER_ALIGNMENT and owned by
    ///          this structure.  Each pixel is 4 consecutive bytes: R, G, B, A.
    ///          Pixel at (x, y) is at pixels[y * stride + x * 4].
    uint8_t *pixels;

    /// @brief Row stride in bytes (always width * 4 for contiguous rows).
    int32_t stride;

    /// @brief Monotonic framebuffer storage generation.
    /// @details Incremented each time pixels is allocated or replaced so direct
    ///          framebuffer users can detect stale descriptors after resize.
    uint64_t framebuffer_generation;

    /// @brief Previous framebuffer allocations retained until window destruction.
    /// @details See `vgfx_retired_framebuffer_t` for the lifetime rationale.
    vgfx_retired_framebuffer_t *retired_framebuffers;

    /// @brief When 1, vgfx_platform_present skips the software framebuffer blit.
    /// @details Set by GPU backends (Metal, D3D11, OpenGL) to prevent the software
    ///          framebuffer CGImage from being drawn on top of GPU-rendered content.
    int8_t skip_software_present;

    //===------------------------------------------------------------------===//
    // Event Queue (Synchronized Ring Buffer)
    //===------------------------------------------------------------------===//

    /// @brief Spin lock protecting event queue indices and overflow count.
    vgfx_atomic_flag_t event_lock;

    /// @brief Ring buffer storage for events.
    /// @details Array of VGFX_INTERNAL_EVENT_QUEUE_SLOTS elements.  The extra
    ///          slot enables full/empty distinction without a separate counter.
    vgfx_event_t event_queue[VGFX_INTERNAL_EVENT_QUEUE_SLOTS];

    /// @brief Next write position.
    /// @details Protected by event_lock. When (head + 1) % SLOTS == tail, the queue is full.
    int32_t event_head;

    /// @brief Next read position.
    /// @details Protected by event_lock. When head == tail, the queue is empty.
    int32_t event_tail;

    /// @brief Count of events dropped since last vgfx_event_overflow_count() call.
    /// @details Protected by event_lock.
    int32_t event_overflow;

    //===------------------------------------------------------------------===//
    // Input State
    //===------------------------------------------------------------------===//

    /// @brief Per-key state array (1 = pressed, 0 = released).
    /// @details Indexed by vgfx_key_t values (must be < 512).  Updated by
    ///          platform backend when processing keyboard events.
    uint8_t key_state[512];

    /// @brief Current mouse X coordinate in window-relative pixels.
    /// @details Updated by platform backend on mouse move events.  May be
    ///          negative or >= width if cursor is outside the window.
    int32_t mouse_x;

    /// @brief Current mouse Y coordinate in window-relative pixels.
    /// @details Updated by platform backend on mouse move events.  May be
    ///          negative or >= height if cursor is outside the window.
    int32_t mouse_y;

    /// @brief Per-button state array (1 = pressed, 0 = released).
    /// @details Indexed by vgfx_mouse_button_t values.  Updated by platform
    ///          backend when processing mouse button events.
    uint8_t mouse_button_state[8];

    /// @brief Relative (raw) mouse mode requested by the application.
    /// @details When non-zero the platform backend delivers unbounded motion
    ///          deltas into relative_dx_accum/relative_dy_accum instead of the
    ///          cursor tracking absolute positions (FPS mouse-look).
    int32_t relative_mouse_enabled;

    /// @brief Whether the platform delivers native raw deltas in relative mode.
    /// @details 0 means the backend cannot provide raw motion (e.g. X11
    ///          without XInput2); callers fall back to warp-to-center capture.
    int32_t relative_mouse_native;

    /// @brief Accumulated relative mouse deltas since the last drain.
    /// @details Written by the platform event handlers under the event lock;
    ///          drained (read-and-clear) by vgfx_get_relative_deltas().
    ///          Units are logical pixels/motion counts (sub-pixel capable).
    double relative_dx_accum;

    /// @copydoc vgfx_window::relative_dx_accum
    double relative_dy_accum;

    //===------------------------------------------------------------------===//
    // Drawing State
    //===------------------------------------------------------------------===//

    /// @brief Whether clipping is enabled (1 = yes, 0 = no).
    /// @details When enabled, all drawing operations are constrained to the
    ///          clip rectangle. When disabled, drawing is constrained to the
    ///          full window bounds.
    int32_t clip_enabled;

    /// @brief Clip rectangle X coordinate (left edge).
    /// @details Only valid when clip_enabled is non-zero.
    int32_t clip_x;

    /// @brief Clip rectangle Y coordinate (top edge).
    /// @details Only valid when clip_enabled is non-zero.
    int32_t clip_y;

    /// @brief Clip rectangle width.
    /// @details Only valid when clip_enabled is non-zero.
    int32_t clip_w;

    /// @brief Clip rectangle height.
    /// @details Only valid when clip_enabled is non-zero.
    int32_t clip_h;

    /// @brief Nested clip-limit scopes (innermost = clip_limit_depth - 1).
    /// @details Each frame stores the effective ceiling for its scope — already
    ///          intersected with every outer scope — plus the exact clip state
    ///          captured when the scope began so the matching pop restores it
    ///          verbatim. While any scope is active, ordinary set/clear clip
    ///          operations cannot enlarge the effective clip beyond the
    ///          innermost ceiling. Fixed-depth and allocation-free.
    struct vgfx_clip_limit_frame {
        /// @brief Physical-pixel ceiling rectangle for this scope.
        int32_t limit_x;
        int32_t limit_y;
        int32_t limit_w;
        int32_t limit_h;
        /// @brief Clip state captured when the scope began, restored by pop.
        int32_t saved_enabled;
        int32_t saved_x;
        int32_t saved_y;
        int32_t saved_w;
        int32_t saved_h;
    } clip_limit_stack[VGFX_CLIP_LIMIT_MAX_DEPTH];

    /// @brief Number of active clip-limit scopes; zero when unlimited.
    int32_t clip_limit_depth;

    //===------------------------------------------------------------------===//
    // Timing
    //===------------------------------------------------------------------===//

    /// @brief Duration of last frame in milliseconds.
    /// @details Updated by vgfx_update() after each frame completes.  Used
    ///          for performance diagnostics and can be queried via
    ///          vgfx_frame_time_ms().
    int64_t last_frame_time_ms;

    /// @brief Absolute timestamp for when the next frame should start.
    /// @details Used by vgfx_frame_pace() for deadline-based frame limiting.
    ///          If fps > 0, pacing sleeps until this deadline and then advances
    ///          it by 1000.0 / fps; lag exceeding one frame resynchronizes the
    ///          deadline to prevent an unbounded catch-up loop.
    double next_frame_deadline_ms;

    //===------------------------------------------------------------------===//
    // Close State
    //===------------------------------------------------------------------===//

    /// @brief Flag set by platform backends when the user clicks the window
    ///        close button (or equivalent).  Checked by the runtime to
    ///        auto-terminate without requiring application-level handling.
    int32_t close_requested;

    /// @brief Non-zero while vgfx_destroy_window is tearing down native resources.
    int32_t destroying;

    /// @brief When non-zero, clicking the close button does not close the window.
    /// @details The close event is still generated so the application can prompt
    ///          the user before actually quitting.
    int32_t prevent_close;

    /// @brief Non-zero when the window has keyboard focus.
    /// @details Updated by the platform backend on focus-gained/lost events.
    int32_t is_focused;

    /// @brief Optional native message hook installed by higher layers.
    /// @details Consulted by platform backends that route native protocol
    ///          messages (Win32 WM_GETOBJECT accessibility today); NULL when
    ///          no hook is installed. See vgfx_set_native_msg_hook().
    vgfx_native_msg_hook_t native_msg_hook;

    /// @brief Opaque user pointer passed back to native_msg_hook.
    void *native_msg_hook_user;

    //===------------------------------------------------------------------===//
    // Resize Callback
    //===------------------------------------------------------------------===//

    /// @brief Optional callback invoked immediately after a window resize.
    /// @details On macOS, the Cocoa live-resize modal loop blocks the main
    ///          thread.  Calling this callback from windowDidResize: allows
    ///          the application to re-render during the drag instead of
    ///          showing a black window.  NULL = no callback.
    void (*on_resize)(void *userdata, int32_t width, int32_t height);

    /// @brief Userdata passed to on_resize.
    void *on_resize_userdata;

    //===------------------------------------------------------------------===//
    // Platform-Specific Data
    //===------------------------------------------------------------------===//

    /// @brief Opaque pointer to platform-specific window data.
    /// @details Allocated and owned by the platform backend.  On macOS, this
    ///          points to a structure containing NSWindow, NSView, etc.  On
    ///          Linux it refers to the selected X11 or Wayland adapter state.
    ///          Must be freed by vgfx_platform_destroy_window().
    void *platform_data;
};

//===----------------------------------------------------------------------===//
// Platform Backend Interface
//===----------------------------------------------------------------------===//
// Each platform backend (vgfx_platform_macos.m, vgfx_platform_linux.c, etc.)
// must provide implementations for the following functions.  The core library
// (vgfx.c) calls these to delegate OS-specific operations.
//===----------------------------------------------------------------------===//

/// @brief Initialize platform-specific window resources.
/// @details Allocates win->platform_data and creates the native OS window.
///          The window should be visible and ready for rendering when this
///          function returns.  On failure, the backend must clean up any
///          partially allocated resources and set a descriptive error via
///          vgfx_internal_set_error().
///
/// @param win    Pointer to the window structure (framebuffer already allocated)
/// @param params Window creation parameters (title, dimensions, flags)
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  params != NULL
/// @pre  win->pixels != NULL (framebuffer already allocated by core)
/// @post On success: win->platform_data != NULL, native window visible
/// @post On failure: win->platform_data == NULL, error set
int vgfx_platform_init_window(struct vgfx_window *win, const vgfx_window_params_t *params);

/// @brief Destroy platform-specific window resources.
/// @details Closes the native OS window and frees win->platform_data.  Must
///          be safe to call even if vgfx_platform_init_window() failed (in
///          which case platform_data may be NULL or partially initialized).
///
/// @param win Pointer to the window structure
///
/// @pre  win != NULL
/// @post win->platform_data == NULL, native window destroyed
void vgfx_platform_destroy_window(struct vgfx_window *win);

/// @brief Process pending OS events and update window state.
/// @details Polls the OS event queue, translates native events into
///          vgfx_event_t, and enqueues them via vgfx_internal_enqueue_event().
///          Also updates win->key_state, win->mouse_x, win->mouse_y, and
///          win->mouse_button_state to reflect the current input state.
///
///          Special handling for CLOSE events: If the user closes the window
///          (clicks the close button), the backend must enqueue a CLOSE event
///          and may return 0 to signal that the window is no longer valid.
///
/// @param win Pointer to the window structure
/// @return 1 on success, 0 on fatal error (e.g., window destroyed)
///
/// @pre  win != NULL
/// @pre  win->platform_data != NULL
/// @post win->key_state, mouse_*, and event queue are updated
int vgfx_platform_process_events(struct vgfx_window *win);

/// @brief Block until an OS event is available for @p win, or the timeout
///        elapses. Does not dequeue or dispatch — callers pump normally after.
/// @param win Window structure.
/// @param timeout_ms Maximum wait in milliseconds (0 = return immediately).
/// @return 1 if events are (probably) available, 0 on timeout.
/// @pre win != NULL && win->platform_data != NULL
int vgfx_platform_wait_events(struct vgfx_window *win, int32_t timeout_ms);

/// @brief Interrupt a platform event wait from another thread.
/// @return 1 when the backend accepted the wake request, otherwise 0.
int vgfx_platform_wake_events(struct vgfx_window *win);

/// @brief Toggle the platform input-method context for an editable control.
/// @details Enables or disables native text composition for the window.
///          Backends may create, focus, or reset their IME context as needed.
/// @param win Window whose input-method context should be updated.
/// @param enabled Non-zero to enable text input, zero to disable it.
/// @return 1 when the requested state was accepted, or 0 on failure.
int vgfx_platform_set_text_input_enabled(struct vgfx_window *win, int32_t enabled);

/// @brief Publish validated, call-scoped editable text state to the platform input method.
/// @details Supplies the current surrounding text, selection, replacement
///          range, and cursor rectangle needed by native composition services.
///          The backend must consume or copy borrowed data during the call.
/// @param win Window whose native input-method state should be updated.
/// @param state Validated text-input snapshot whose borrowed fields remain
///              valid only for the duration of this call.
/// @return 1 when the state was accepted, or 0 on failure.
int vgfx_platform_set_text_input_state(struct vgfx_window *win,
                                       const vgfx_text_input_state_t *state);

/// @brief Present (blit) the framebuffer to the native window.
/// @details Transfers the contents of win->pixels to the OS window surface
///          so they become visible on screen.  This may involve creating a
///          platform-specific bitmap, copying pixel data, and invalidating
///          the window's view to trigger a redraw.
///
/// @param win Pointer to the window structure
/// @return 1 on success, 0 on failure
///
/// @pre  win != NULL
/// @pre  win->pixels != NULL
/// @pre  win->platform_data != NULL
/// @post Framebuffer contents are visible in the native window
int vgfx_platform_present(struct vgfx_window *win);

/// @brief Allocate aligned storage for a framebuffer.
/// @details Implemented by each platform adapter so core code does not need
///          platform preprocessor branches for aligned allocation.  Returned
///          storage is uninitialized and must be released with
///          `vgfx_platform_aligned_free()`.
/// @param alignment Required byte alignment, power of two.  Backends may also
///                  require the platform's minimum pointer alignment.
/// @param size Number of bytes to allocate.
/// @return Aligned allocation, or NULL on failure.
void *vgfx_platform_aligned_alloc(size_t alignment, size_t size);

/// @brief Free storage returned by vgfx_platform_aligned_alloc().
/// @details Dispatches to the platform-specific deallocator that matches the
///          allocation routine above.  Passing NULL is a no-op.
/// @param ptr Pointer returned by vgfx_platform_aligned_alloc(), or NULL.
void vgfx_platform_aligned_free(void *ptr);

/// @brief Sleep for the specified duration.
/// @details Used by vgfx_frame_pace() for frame rate limiting.  The actual sleep
///          duration may be slightly longer due to OS scheduler granularity.
///          If ms <= 0, this function returns immediately without sleeping.
///
/// @param ms Duration to sleep in milliseconds
void vgfx_platform_sleep_ms(int32_t ms);

/// @brief Yield the current thread without advancing frame time.
/// @details Used by short spin-wait paths such as the event queue lock.  Backends
///          should prefer a native scheduler yield where available and may fall
///          back to a minimal sleep on platforms without a zero-duration yield.
void vgfx_platform_yield(void);

/// @brief Yield while waiting for the event queue lock.
/// @details Called by the internal event queue spin lock when another thread
///          currently owns the queue.  Delegates to vgfx_platform_yield() so
///          native event producers and the application consumer do not burn a
///          full CPU core during brief contention.
void vgfx_internal_event_wait(void);

/// @brief Get current high-resolution timestamp in milliseconds.
/// @details Returns a monotonic timestamp (never decreases) with millisecond
///          precision.  The epoch is arbitrary but consistent within a process.
///          Used for frame timing and FPS limiting.
///
/// @return Milliseconds since an arbitrary epoch (monotonic)
///
/// @post Return value >= previous calls within the same process
int64_t vgfx_platform_now_ms(void);

/// @brief Set the window title.
/// @details Updates the native OS window's title bar text.
///
/// @param win   Pointer to the window structure
/// @param title New title string (UTF-8), or NULL for default
///
/// @pre  win != NULL
/// @pre  win->platform_data != NULL
void vgfx_platform_set_title(struct vgfx_window *win, const char *title);

/// @brief Set the application / window icon from 0xRRGGBBAA words (ADR 0317).
/// @details Backends without an icon concept (Wayland, mock) ignore the call.
///
/// @param win    Pointer to the window structure
/// @param rgba   Row-major pixel words (validated non-NULL by vgfx_set_icon)
/// @param width  Icon width in pixels
/// @param height Icon height in pixels
void vgfx_platform_set_icon(struct vgfx_window *win,
                            const uint32_t *rgba,
                            int32_t width,
                            int32_t height);

/// @brief Set the window to fullscreen or windowed mode.
/// @details Toggles the native OS window between fullscreen and windowed modes.
///          The framebuffer should be reallocated by the caller if dimensions change.
///
/// @param win        Pointer to the window structure
/// @param fullscreen 1 for fullscreen, 0 for windowed
///
/// @pre  win != NULL
/// @pre  win->platform_data != NULL
/// @return 1 on success, 0 on failure
int vgfx_platform_set_fullscreen(struct vgfx_window *win, int fullscreen);

/// @brief Check if the window is in fullscreen mode.
/// @param win Pointer to the window structure
/// @return 1 if fullscreen, 0 if windowed
int vgfx_platform_is_fullscreen(struct vgfx_window *win);

/// @brief Minimize (iconify) the native window.
/// @param win Window to minimize.
void vgfx_platform_minimize(struct vgfx_window *win);

/// @brief Maximize (zoom) the native window.
/// @param win Window to maximize.
void vgfx_platform_maximize(struct vgfx_window *win);

/// @brief Restore the native window from minimized or maximized state.
/// @param win Window to restore.
void vgfx_platform_restore(struct vgfx_window *win);

/// @brief Check if the native window is currently minimized.
/// @param win Window whose native minimized state should be queried.
/// @return 1 if minimized, 0 otherwise
int32_t vgfx_platform_is_minimized(struct vgfx_window *win);

/// @brief Check if the native window is currently maximized.
/// @param win Window whose native maximized state should be queried.
/// @return 1 if maximized, 0 otherwise
int32_t vgfx_platform_is_maximized(struct vgfx_window *win);

/// @brief Query the HiDPI backing scale factor from the host display system.
/// @details Called once in vgfx_create_window() before framebuffer allocation.
///          Returns 1.0 on non-HiDPI displays; 2.0 on standard macOS Retina;
///          varies on Linux (Xft.dpi / 96.0) and Windows (GetDeviceCaps / 96.0).
/// @return Positive physical-pixels-per-logical-unit scale, normally at least
///         1.0; backends fall back to 1.0 when no display scale is available.
float vgfx_platform_get_display_scale(void);

/// @brief Get the native window's current screen position.
/// @param win Window whose position should be queried.
/// @param out_x Receives the native screen-space X coordinate when non-NULL.
/// @param out_y Receives the native screen-space Y coordinate when non-NULL.
void vgfx_platform_get_position(struct vgfx_window *win, int32_t *out_x, int32_t *out_y);

/// @brief Move the native window to a new screen position.
/// @param win Window to move.
/// @param x Requested native screen-space X coordinate.
/// @param y Requested native screen-space Y coordinate.
void vgfx_platform_set_position(struct vgfx_window *win, int32_t x, int32_t y);

/// @brief Give keyboard focus to the native window.
/// @param win Window that should receive keyboard focus.
void vgfx_platform_focus(struct vgfx_window *win);

/// @brief Request foreground activation for the native application/window.
/// @param win Window for which foreground activation is requested.
void vgfx_platform_request_foreground(struct vgfx_window *win);

/// @brief Check if the native window has keyboard focus.
/// @param win Window whose native focus state should be queried.
/// @return 1 if focused, 0 otherwise
int32_t vgfx_platform_is_focused(struct vgfx_window *win);

/// @brief Control whether clicking the native close button closes the window.
/// @param win Window whose native close behavior should be updated.
/// @param prevent Non-zero to intercept native close requests, zero to permit
///                the backend's normal close behavior.
void vgfx_platform_set_prevent_close(struct vgfx_window *win, int32_t prevent);

/// @brief Set the native mouse cursor shape.
/// @param win Window whose cursor should be changed.
/// @param cursor_type `VGFX_CURSOR_*` shape constant.
void vgfx_platform_set_cursor(struct vgfx_window *win, int32_t cursor_type);

/// @brief Show or hide the native mouse cursor.
/// @param win Window whose cursor visibility should be changed.
/// @param visible Non-zero to show the cursor, zero to hide it.
void vgfx_platform_set_cursor_visible(struct vgfx_window *win, int32_t visible);

/// @brief Hide the process or current-window cursor through the active platform backend.
void vgfx_platform_hide_cursor(void);

/// @brief Show a cursor previously hidden through the active platform backend.
void vgfx_platform_show_cursor(void);

#ifdef __APPLE__
/// @brief Ensure the Cocoa app is finished launching before menu installation.
/// @details Safe to call repeatedly. Used by the macOS platform backend and
///          the GUI menubar bridge before setting NSApp's main menu.
void vgfx_platform_macos_finish_launching_if_needed(void);

/// @brief Install the standard macOS app/window menus as the active main menu.
/// @details Replaces any current main menu with the default Zanna menu set.
///          preferred_title is used for the application menu title when non-empty.
/// @param preferred_title Preferred UTF-8 application-menu title, or NULL or
///                        empty to derive the process's default display name.
void vgfx_platform_macos_install_default_main_menu(const char *preferred_title);

/// @brief Ensure a default macOS main menu exists without clobbering custom menus.
/// @details If the current main menu is absent or is already the default Zanna
///          menu, this rebuilds it. If a custom menu is active, it is preserved.
/// @param preferred_title Preferred UTF-8 application-menu title, or NULL or
///                        empty to derive the process's default display name.
void vgfx_platform_macos_ensure_default_main_menu(const char *preferred_title);
#endif

/// @brief Get the screen dimensions of the monitor containing the window.
/// @param win Window selecting the monitor to query.
/// @param out_w Receives the monitor width in native screen units when non-NULL.
/// @param out_h Receives the monitor height in native screen units when non-NULL.
void vgfx_platform_get_monitor_size(struct vgfx_window *win, int32_t *out_w, int32_t *out_h);

/// @brief Resize the native OS window.
/// @param win Window whose client/content area should be resized.
/// @param w Requested logical client/content width.
/// @param h Requested logical client/content height.
void vgfx_platform_set_window_size(struct vgfx_window *win, int32_t w, int32_t h);

/// @brief Publish the minimum logical client/content size to the native window manager.
/// @param win Window whose resize constraints should be updated.
/// @param w Minimum logical client/content width.
/// @param h Minimum logical client/content height.
void vgfx_platform_set_window_min_size(struct vgfx_window *win, int32_t w, int32_t h);

//===----------------------------------------------------------------------===//
// Internal Helper Functions
//===----------------------------------------------------------------------===//
// These functions are implemented in vgfx.c and used internally by the core
// library and platform backends.  They are NOT part of the public API.
//===----------------------------------------------------------------------===//

/// @brief Set the thread-local error code and message.
/// @details Stores the error information so it can be retrieved via
///          `vgfx_last_error_code()` and `vgfx_get_last_error()`.  This function
///          is called by both the core library and platform backends when an
///          error occurs.  The message is copied into thread-local storage.
///
/// @param code Error code (enum vgfx_error_t)
/// @param msg  Descriptive error message (UTF-8, not NULL)
///
/// @pre  msg != NULL
/// @post `vgfx_last_error_code()` returns @p code and
///       `vgfx_get_last_error()` returns the copied message.
void vgfx_internal_set_error(vgfx_error_t code, const char *msg);

/// @brief Initialize one allocation-free native IME composition event.
/// @details Clears the complete event value, validates the lifecycle type, copies at most
///          `VGFX_COMPOSITION_TEXT_CAPACITY - 1` bytes, backs up to a UTF-8 codepoint boundary
///          when truncation is required, and stores all selection/replacement metadata. Native
///          adapters should pass valid UTF-8; this helper guarantees bounded storage and a NUL
///          terminator but deliberately does not perform Unicode normalization.
/// @param event Destination value to initialize; must not be NULL.
/// @param type One of the four `VGFX_EVENT_COMPOSITION_*` lifecycle types.
/// @param time_ms Monotonic event timestamp in milliseconds.
/// @param text Borrowed UTF-8 bytes; NULL is accepted only with @p text_length equal to zero.
/// @param text_length Number of readable bytes at @p text, excluding any terminator.
/// @param selection_start Preedit selection start in Unicode codepoints.
/// @param selection_length Preedit selection length in Unicode codepoints.
/// @param replacement_start Committed-text codepoint start, or -1 for current selection.
/// @param replacement_length Committed-text codepoint length, or -1 with sentinel start.
/// @param modifiers Active `VGFX_MOD_*` mask.
/// @return One when initialized; zero for a NULL destination, invalid type, or invalid text pair.
int vgfx_internal_init_composition_event(vgfx_event_t *event,
                                         vgfx_event_type_t type,
                                         int64_t time_ms,
                                         const char *text,
                                         size_t text_length,
                                         int32_t selection_start,
                                         int32_t selection_length,
                                         int32_t replacement_start,
                                         int32_t replacement_length,
                                         int modifiers);

/// @brief Enqueue an event into the window's synchronized ring buffer.
/// @details Consecutive composition-update events are coalesced.  If the queue
///          is full, a queued event is evicted only when doing so preserves
///          important state transitions: CLOSE and release-state events are
///          retained whenever possible, while transient motion, scrolling,
///          resize, and text events are preferred victims.  When no safe victim
///          exists, the new event is dropped.  Each full-queue eviction or drop
///          increments the saturating overflow counter.
///
///          This function is safe to call from platform event callbacks.
///
/// @param win   Pointer to the window structure
/// @param event Pointer to the event to enqueue (copied into the queue)
/// @return 1 if the event was enqueued or coalesced, or 0 for invalid input,
///         destruction in progress, or a full queue with no safe victim.
///
/// @pre  win != NULL
/// @pre  event != NULL
/// @post On a full queue, the event is stored or event_overflow is incremented.
int vgfx_internal_enqueue_event(struct vgfx_window *win, const vgfx_event_t *event);

/// @brief Enqueue an event while coalescing consecutive mouse-motion updates.
/// @details High-frequency native mouse motion can flood the fixed-size event
///          queue.  When the newest queued event is already a mouse-move event,
///          this helper replaces it with the latest coordinates instead of
///          appending another redundant move.  Non-motion events use the regular
///          enqueue semantics.
/// @param win Window whose queue receives the event.
/// @param event Event to enqueue or coalesce.
/// @return 1 when the event was queued/coalesced, 0 when it was dropped.
int vgfx_internal_enqueue_coalesced_event(struct vgfx_window *win, const vgfx_event_t *event);

/// @brief Record a platform-side event that had to be dropped before enqueue.
/// @details Used when a backend rejects an oversized or otherwise unsafe native
///          event payload before it can be represented as a vgfx_event_t.  The
///          window's saturating overflow counter is updated under the event lock.
/// @param win Window whose overflow counter should be incremented; NULL is ignored.
void vgfx_internal_note_event_overflow(struct vgfx_window *win);

/// @brief Dequeue the next event from the window's ring buffer.
/// @details Removes and returns the oldest event from the queue.  If the queue
///          is empty, returns 0 without modifying out_event.
///
///          This function is safe to call from the application thread (consumer).
///
/// @param win       Pointer to the window structure
/// @param out_event Pointer to event storage (filled on success)
/// @return 1 if an event was dequeued, 0 if queue is empty
///
/// @pre  win != NULL
/// @pre  out_event != NULL
/// @post If 1 returned: out_event contains the next event, event_tail advanced
/// @post If 0 returned: out_event unchanged, queue is empty
int vgfx_internal_dequeue_event(struct vgfx_window *win, vgfx_event_t *out_event);

/// @brief Peek at the next event without removing it from the queue.
/// @details Returns the oldest event without advancing event_tail.  Useful for
///          checking if a specific event type is pending without consuming it.
///
/// @param win       Pointer to the window structure
/// @param out_event Pointer to event storage (filled on success)
/// @return 1 if an event is available, 0 if queue is empty
///
/// @pre  win != NULL
/// @pre  out_event != NULL
/// @post If 1 returned: out_event contains the next event, queue unchanged
/// @post If 0 returned: out_event unchanged, queue is empty
int vgfx_internal_peek_event(struct vgfx_window *win, vgfx_event_t *out_event);

/// @brief Reallocate and clear the shared RGBA framebuffer for a new size.
/// @details Backends call this from resize handlers before enqueuing
///          VGFX_EVENT_RESIZE so the public framebuffer contract stays true on
///          every platform.
/// @param win Pointer to the window structure
/// @param width New physical framebuffer width in pixels
/// @param height New physical framebuffer height in pixels
/// @return 1 on success, 0 on allocation or validation failure
int vgfx_internal_resize_framebuffer(struct vgfx_window *win, int32_t width, int32_t height);

/// @brief Clear all sticky keyboard and mouse button polling state for a window.
/// @details Platform backends call this when the native window loses focus so
///          polling APIs cannot report keys/buttons as held forever after the
///          operating system stops sending release events to the window.  The
///          reset occurs while holding the shared event/state lock.
/// @param win Window whose key, button, and relative-motion state should be
///            reset; NULL is ignored.
void vgfx_internal_clear_input_state(struct vgfx_window *win);

/// @brief Set key polling state for a window.
/// @details Validates the public key range, acquires the window event/state
///          lock, and updates the sticky polling state used by
///          `vgfx_key_down()`.  Invalid keys are ignored.
/// @param win Window whose key state should be updated.
/// @param key Zanna key code to update.
/// @param down Non-zero when pressed, zero when released.
void vgfx_internal_set_key_state(struct vgfx_window *win, int32_t key, int32_t down);

/// @brief Set mouse-button polling state for a window.
/// @details Validates the button index, acquires the window event/state lock,
///          and updates the sticky polling state used by
///          `vgfx_mouse_button()`.
/// @param win Window whose mouse-button state should be updated.
/// @param button Mouse button index.
/// @param down Non-zero when pressed, zero when released.
void vgfx_internal_set_mouse_button_state(struct vgfx_window *win, int32_t button, int32_t down);

/// @brief Store the latest raw framebuffer-space mouse position.
/// @details Acquires the window event/state lock before updating the cached
///          mouse coordinates.  Public getters convert from this raw coordinate
///          space according to the current coordinate scale.
/// @param win Window whose mouse coordinates should be updated.
/// @param x Raw physical X coordinate.
/// @param y Raw physical Y coordinate.
void vgfx_internal_set_mouse_position(struct vgfx_window *win, int32_t x, int32_t y);

/// @brief Accumulate a relative (raw) mouse motion delta.
/// @details Acquires the window event/state lock before adding to the
///          relative accumulators drained by `vgfx_get_relative_deltas()`.
///          Platform event handlers call this while relative mouse mode is
///          enabled; deltas are sub-pixel capable doubles in logical units.
/// @param win Window receiving the motion.
/// @param dx Horizontal motion since the previous event.
/// @param dy Vertical motion since the previous event (positive = down).
void vgfx_internal_add_relative_delta(struct vgfx_window *win, double dx, double dy);

/// @brief Set the close-requested polling flag.
/// @details Acquires the window event/state lock before updating the flag read
///          by `vgfx_close_requested()`.
/// @param win Window whose close state should be updated.
/// @param requested Non-zero when a close was requested, zero to clear.
void vgfx_internal_set_close_requested(struct vgfx_window *win, int32_t requested);

/// @brief Set the cached focus flag.
/// @details Acquires the window event/state lock before updating the focus
///          state used by platform focus queries and focus-lost cleanup.
/// @param win Window whose focus state should be updated.
/// @param focused Non-zero when focused, zero when unfocused.
void vgfx_internal_set_focus_state(struct vgfx_window *win, int32_t focused);

/// @brief Set whether close requests should be prevented.
/// @details Acquires the window event/state lock before updating the flag used
///          by native close-button handlers.
/// @param win Window whose close-prevention flag should be updated.
/// @param prevent Non-zero to prevent close requests, zero to allow them.
void vgfx_internal_set_prevent_close(struct vgfx_window *win, int32_t prevent);

/// @brief Acquire a window's shared event and input-state spin lock.
/// @details Spins on the internal atomic flag, using a lightweight scheduler
///          yield for short contention and a one-millisecond sleep every 64
///          unsuccessful attempts to avoid monopolizing a CPU.  The matching
///          `vgfx_internal_event_unlock()` must release the lock.
/// @param win Valid window whose event lock should be acquired.
/// @pre win != NULL.
/// @post The calling thread owns `win->event_lock`.
static inline void vgfx_internal_event_lock(struct vgfx_window *win) {
    uint32_t spins = 0;
    while (vgfx_atomic_flag_test_and_set(&win->event_lock)) {
        spins++;
        if ((spins & 63u) == 0u)
            vgfx_platform_sleep_ms(1);
        else
            vgfx_internal_event_wait();
    }
}

/// @brief Release a window's shared event and input-state spin lock.
/// @param win Valid window whose event lock is owned by the calling thread.
/// @pre win != NULL and the caller owns `win->event_lock`.
static inline void vgfx_internal_event_unlock(struct vgfx_window *win) {
    vgfx_atomic_flag_clear(&win->event_lock);
}

/// @brief Check if pixel coordinates are within the window's bounds.
/// @details Fast bounds check for drawing operations.  Returns true if the
///          pixel at (x, y) is inside the framebuffer [0, width) × [0, height).
///
/// @param win Pointer to the window structure (may be NULL, returns 0)
/// @param x   X coordinate in pixels
/// @param y   Y coordinate in pixels
/// @return 1 if (x, y) is in bounds, 0 otherwise
///
/// @post Return value is 1 iff win != NULL && 0 <= x < width && 0 <= y < height
static inline int vgfx_internal_in_bounds(struct vgfx_window *win, int32_t x, int32_t y) {
    return (win && x >= 0 && x < win->width && y >= 0 && y < win->height);
}

/// @brief Test whether a physical pixel survives framebuffer and clip bounds.
/// @details First rejects coordinates outside the framebuffer.  With clipping
///          disabled, every remaining pixel is accepted.  With clipping
///          enabled, an empty rectangle rejects all pixels and otherwise uses
///          half-open 64-bit edge calculations to avoid signed overflow.
/// @param win Window providing framebuffer and effective clip state; NULL
///            rejects the coordinate.
/// @param x Physical framebuffer X coordinate.
/// @param y Physical framebuffer Y coordinate.
/// @return 1 when the pixel is drawable under the active clip, otherwise 0.
static inline int vgfx_internal_in_effective_clip(struct vgfx_window *win, int32_t x, int32_t y) {
    if (!vgfx_internal_in_bounds(win, x, y))
        return 0;
    if (!win->clip_enabled)
        return 1;
    if (win->clip_w <= 0 || win->clip_h <= 0)
        return 0;

    int64_t clip_left = (int64_t)win->clip_x;
    int64_t clip_top = (int64_t)win->clip_y;
    int64_t clip_right = clip_left + (int64_t)win->clip_w;
    int64_t clip_bottom = clip_top + (int64_t)win->clip_h;
    return (int64_t)x >= clip_left && (int64_t)x < clip_right && (int64_t)y >= clip_top &&
           (int64_t)y < clip_bottom;
}

#define VGFX_MAX_SCALE_FACTOR 16.0f

/// @brief Normalize a requested display or coordinate scale.
/// @details Non-finite values and values below one fall back to 1.0.  Larger
///          finite values are capped at `VGFX_MAX_SCALE_FACTOR` so later
///          coordinate multiplication remains bounded.
/// @param scale Candidate physical-pixels-per-logical-unit scale.
/// @return Sanitized scale in the inclusive range [1.0, 16.0].
static inline float vgfx_internal_sanitize_scale(float scale) {
    if (!isfinite(scale) || scale < 1.0f)
        return 1.0f;
    if (scale > VGFX_MAX_SCALE_FACTOR)
        return VGFX_MAX_SCALE_FACTOR;
    return scale;
}

/// @brief Round a finite scaled coordinate to the nearest signed integer.
/// @details Uses half-away-from-zero rounding and saturates values outside the
///          `int32_t` range.  Non-finite input maps to zero.
/// @param value Floating-point coordinate or extent to convert.
/// @return Rounded and saturated 32-bit integer.
static inline int32_t vgfx_internal_round_scaled(float value) {
    if (!isfinite(value))
        return 0;
    if (value >= (float)INT32_MAX)
        return INT32_MAX;
    if (value <= (float)INT32_MIN)
        return INT32_MIN;
    return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

/// @brief Obtain a window's sanitized public-coordinate scale.
/// @param win Window whose coordinate scale should be read, or NULL.
/// @return Sanitized `win->coord_scale`, or 1.0 for a NULL window.
static inline float vgfx_internal_coord_scale(const struct vgfx_window *win) {
    return win ? vgfx_internal_sanitize_scale(win->coord_scale) : 1.0f;
}

/// @brief Convert a logical integer coordinate to physical pixels.
/// @details Sanitizes @p scale, multiplies in floating point, and applies the
///          internal saturating half-away-from-zero rounding rule.
/// @param logical Logical coordinate or extent.
/// @param scale Requested physical-pixels-per-logical-unit scale.
/// @return Rounded physical-pixel value.
static inline int32_t vgfx_internal_scale_up_i32(int32_t logical, float scale) {
    return vgfx_internal_round_scaled((float)logical * vgfx_internal_sanitize_scale(scale));
}

/// @brief Convert a physical-pixel integer to logical coordinates.
/// @details Divides by a sanitized scale and applies the internal saturating
///          half-away-from-zero rounding rule.
/// @param physical Physical framebuffer coordinate or extent.
/// @param scale Requested physical-pixels-per-logical-unit scale.
/// @return Rounded logical-coordinate value.
static inline int32_t vgfx_internal_scale_down_i32(int32_t physical, float scale) {
    return vgfx_internal_round_scaled((float)physical / vgfx_internal_sanitize_scale(scale));
}

/// @brief Convert a framebuffer dimension to the public coordinate-space size.
/// @details The public size APIs and resize event logical fields report the
///          framebuffer dimension divided by the active coordinate scale.  When
///          the GUI layer leaves coord_scale at 1.0 this is the physical
///          framebuffer size; when the Canvas layer sets coord_scale to the
///          HiDPI backing scale it is the logical drawing size.
/// @param framebuffer_extent Physical framebuffer width or height in pixels.
/// @param win Window whose coordinate scale should be applied.
/// @return Public coordinate-space extent after scale conversion.
static inline int32_t vgfx_internal_public_extent_i32(int32_t framebuffer_extent,
                                                      const struct vgfx_window *win) {
    return vgfx_internal_scale_down_i32(framebuffer_extent, vgfx_internal_coord_scale(win));
}

/// @brief Refresh a window's backing scale without overriding an explicit coordinate scale.
/// @details Stores sanitized @p new_scale in `scale_factor`.  A caller that
///          selected the coordinate scale and left it tracking the previous
///          backing scale — within a small tolerance for representation noise —
///          has it advanced to the new value as well; a caller-selected
///          independent coordinate scale is preserved.
///
///          The untouched 1.0 default is never advanced. It means "public
///          coordinates are physical pixels", which is what the GUI layer
///          relies on, and it is numerically indistinguishable from an explicit
///          1.0 selection. Backends that learn the HiDPI factor only after the
///          window exists (Wayland reports it asynchronously) would otherwise
///          flip every GUI window into coordinate-scaled mode on the first
///          scale report, so `vgfx_get_size` would start returning logical
///          extents that the GUI layer then divides by the scale a second time.
/// @param win Window whose scale state should be refreshed; NULL is ignored.
/// @param new_scale Newly reported physical-pixels-per-logical-unit scale.
static inline void vgfx_internal_refresh_scale_factor(struct vgfx_window *win, float new_scale) {
    if (!win)
        return;

    float old_scale = vgfx_internal_sanitize_scale(win->scale_factor);
    float old_coord = vgfx_internal_coord_scale(win);
    float sanitized = vgfx_internal_sanitize_scale(new_scale);

    win->scale_factor = sanitized;

    if (!win->coord_scale_explicit)
        return;

    if (old_coord == old_scale || (old_coord > old_scale - 0.01f && old_coord < old_scale + 0.01f))
        win->coord_scale = sanitized;
}

/// @brief Initialize a resize event from physical framebuffer dimensions.
/// @details Sets the event type and timestamp, records the physical width and
///          height, and derives logical dimensions through the window's active
///          coordinate scale.  Other union members are not cleared.
/// @param event Resize event to initialize; NULL is ignored.
/// @param win Window whose public coordinate scale should be used; NULL selects
///            the neutral 1.0 scale.
/// @param time_ms Monotonic event timestamp in milliseconds.
/// @param framebuffer_width New physical framebuffer width.
/// @param framebuffer_height New physical framebuffer height.
static inline void vgfx_internal_init_resize_event(vgfx_event_t *event,
                                                   struct vgfx_window *win,
                                                   int64_t time_ms,
                                                   int32_t framebuffer_width,
                                                   int32_t framebuffer_height) {
    if (!event)
        return;

    event->type = VGFX_EVENT_RESIZE;
    event->time_ms = time_ms;
    event->data.resize.width = framebuffer_width;
    event->data.resize.height = framebuffer_height;
    event->data.resize.logical_width = vgfx_internal_public_extent_i32(framebuffer_width, win);
    event->data.resize.logical_height = vgfx_internal_public_extent_i32(framebuffer_height, win);
}

/// @brief Test whether a Unicode scalar lies in a private-use area.
/// @details Recognizes the Basic Multilingual Plane private-use block and the
///          two supplementary private-use planes through their last assigned
///          scalar values.
/// @param codepoint Candidate Unicode scalar value.
/// @return 1 for a private-use code point, otherwise 0.
static inline int vgfx_internal_codepoint_is_private_use(uint32_t codepoint) {
    return (codepoint >= 0xE000 && codepoint <= 0xF8FF) ||
           (codepoint >= 0xF0000 && codepoint <= 0xFFFFD) ||
           (codepoint >= 0x100000 && codepoint <= 0x10FFFD);
}

/// @brief Test whether a Unicode value is suitable for text-input events.
/// @details Rejects C0/C1 controls, DELETE, surrogate code points, values above
///          Unicode's maximum scalar, and private-use characters reserved for
///          platform-specific key representations.
/// @param codepoint Candidate Unicode value.
/// @return 1 for an accepted textual scalar, otherwise 0.
static inline int vgfx_internal_codepoint_is_text(uint32_t codepoint) {
    if (codepoint < 0x20 || codepoint == 0x7F || codepoint > 0x10FFFF)
        return 0;
    if (codepoint >= 0x80 && codepoint <= 0x9F)
        return 0;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
        return 0;
    if (vgfx_internal_codepoint_is_private_use(codepoint))
        return 0;
    return 1;
}

/// @brief Determine whether active command modifiers permit text emission.
/// @details Command always suppresses text.  Control or Alt alone also
///          suppresses text, while the AltGr-style Control+Alt combination is
///          allowed so layouts can produce their alternate characters.
/// @param modifiers Bitwise `VGFX_MOD_*` mask.
/// @return 1 when modifiers permit text input, otherwise 0.
static inline int vgfx_internal_text_modifiers_allow_text(int modifiers) {
    int has_cmd = (modifiers & VGFX_MOD_CMD) != 0;
    int has_ctrl = (modifiers & VGFX_MOD_CTRL) != 0;
    int has_alt = (modifiers & VGFX_MOD_ALT) != 0;

    if (has_cmd)
        return 0;
    if (has_ctrl && !has_alt)
        return 0;
    if (has_alt && !has_ctrl)
        return 0;
    return 1;
}

/// @brief Decide whether a translated key should generate a text-input event.
/// @param codepoint Candidate Unicode value produced by the native key event.
/// @param modifiers Active bitwise `VGFX_MOD_*` mask.
/// @return 1 only when both the code point and modifier combination are
///         acceptable for textual input.
static inline int vgfx_internal_should_emit_text_input(uint32_t codepoint, int modifiers) {
    return vgfx_internal_codepoint_is_text(codepoint) &&
           vgfx_internal_text_modifiers_allow_text(modifiers);
}

//===----------------------------------------------------------------------===//
// End of Internal Definitions
//===----------------------------------------------------------------------===//
