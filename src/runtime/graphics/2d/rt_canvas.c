//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_canvas.c
/// @file
/// @brief Implements the 2D Canvas window lifecycle, presentation timing,
///        event dispatch, clipping, and native-window controls.
// Purpose: Canvas lifecycle and window management functions. Handles creation,
//   destruction, event polling, resize, fullscreen, window position/focus,
//   and screenshot/save operations.
//
// Key invariants:
//   - Public operations validate the Canvas handle; most closed/null operations
//     return documented fallbacks or become no-ops.
//   - rt_canvas_flip() presents the back-buffer and must be called each frame.
//   - rt_canvas_poll() drives the event loop and returns the last event type processed.
//   - Logical dimensions and input coordinates are kept separate from the
//     backend's physical HiDPI framebuffer coordinates.
//
// Ownership/Lifetime:
//   - rt_canvas objects are reference-counted via rt_obj_new_i64.
//     rt_canvas_destroy() releases one owned reference.
//   - The finalizer owns and destroys gfx_win, frees the copied title, and
//     detaches global input-module window references.
//
// Links: src/runtime/graphics/common/rt_graphics_internal.h,
//        src/runtime/graphics/common/rt_graphics.h (public API),
//        vgfx.h (ZannaGFX C API)
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_internal.h"
#include "rt_platform.h"
#include "rt_time.h"

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Validate that an int64 canvas dimension is positive and fits in int32; traps otherwise.
/// @param value Requested logical dimension.
/// @param op Trap diagnostic used for invalid input.
/// @return Valid positive int32 dimension, or `0` if the trap hook returns.
static int32_t rt_canvas_dimension_to_i32(int64_t value, const char *op) {
    if (value <= 0 || value > INT32_MAX) {
        rt_trap(op);
        return 0;
    }
    return (int32_t)value;
}

/// @brief Clear keyboard/mouse module references that point at this window.
/// @details The input modules cache the active canvas so global queries like
///          `Action.Held()` can route to the right vgfx window. When a canvas
///          is destroyed, those caches must be invalidated or subsequent
///          input polls would read freed memory. The "if matches" guards
///          ensure we don't clobber input state belonging to another canvas
///          when multiple windows are open.
/// @param gfx_win Borrowed backend window; `NULL` is ignored.
static void rt_canvas_detach_input(vgfx_window_t gfx_win) {
    if (!gfx_win)
        return;
    rt_keyboard_clear_canvas_if_matches(gfx_win);
    rt_mouse_clear_canvas_if_matches(gfx_win);
}

/// @brief Detach input bindings, destroy the underlying vgfx window, and clear the pointer.
/// @details Single chokepoint for window teardown so both the explicit
///          `rt_canvas_close` path and the GC finalizer follow the same
///          ordering: detach input first (so no late event delivery into
///          the doomed window), then destroy, then null the pointer so
///          subsequent ops see a closed canvas.
/// @param canvas Borrowed Canvas implementation; null/closed objects are ignored.
static void rt_canvas_destroy_window(rt_canvas *canvas) {
    int expected_loan_state = 0;
    if (!canvas)
        return;
    if (!rt_atomic_compare_exchange_i32(&canvas->window_loan_active,
                                        &expected_loan_state,
                                        2,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
        if (expected_loan_state == 1)
            rt_trap("Canvas.Close: window is adopted by Canvas3D");
        return;
    }
    if (!canvas->gfx_win)
        return;
    /* Restore normal cursor behavior before the window goes away — relative
     * mouse mode is a per-process setting on some platforms (macOS cursor
     * dissociation) and must not outlive the window. */
    if (canvas->relative_mouse_applied) {
        (void)vgfx_set_relative_mouse(canvas->gfx_win, 0);
        rt_mouse_set_relative_native(0);
        canvas->relative_mouse_applied = 0;
    }
    rt_canvas_detach_input(canvas->gfx_win);
    vgfx_destroy_window(canvas->gfx_win);
    canvas->gfx_win = NULL;
    canvas->window_state_synced = 0;
}

/// @brief GC finalizer: destroy window, free cached title, and clear the magic number.
/// @details Invoked by the runtime when the canvas refcount drops to zero.
///          Wiping `magic` lets `rt_canvas_checked` reject use-after-free
///          (caller dereferences a stale handle) by detecting the zeroed
///          sentinel rather than producing undefined behavior.
/// @param obj Canvas object being finalized; `NULL` is ignored.
static void rt_canvas_finalize(void *obj) {
    if (!obj)
        return;

    rt_canvas *canvas = (rt_canvas *)obj;
    canvas->magic = 0;
    rt_canvas_destroy_window(canvas);
    if (canvas->title) {
        free(canvas->title);
        canvas->title = NULL;
    }
    canvas->title_len = 0;
}

/// @brief Convert a physical pixel position from a vgfx event into logical mouse coords.
/// @details All public Canvas drawing uses logical pixels (CSS pixels);
///          vgfx mouse events arrive in physical pixels (real framebuffer
///          coordinates), which are 2x larger on Retina/HiDPI displays.
///          Dividing by the per-window scale factor keeps `Mouse.X/Y`
///          consistent with the coordinate space the user draws in. The
///          `< 0.001f` guard avoids division by an uninitialized scale.
/// @param canvas Borrowed live Canvas.
/// @param x Physical framebuffer X from the backend event.
/// @param y Physical framebuffer Y from the backend event.
static void rt_canvas_update_mouse_from_physical(rt_canvas *canvas, int32_t x, int32_t y) {
    if (!canvas || !canvas->gfx_win)
        return;
    float scale = rt_canvas_effective_coord_scale(canvas);
    if (scale < 0.001f)
        scale = 1.0f;
    rt_mouse_update_pos((int64_t)((double)x / (double)scale), (int64_t)((double)y / (double)scale));
}

/// @brief Report that Canvas support is compiled into this runtime.
/// @return Always `1` in a `ZANNA_ENABLE_GRAPHICS` build.
int8_t rt_canvas_is_available(void) {
    return 1;
}

/// @brief Create a new Canvas window with the given title and dimensions.
/// @details Allocates a GC-managed rt_canvas struct, initializes the ZannaGFX
///   window backend, sets up HiDPI coordinate scaling, and initializes keyboard,
///   mouse, and gamepad input subsystems. The canvas is ready for drawing after
///   this call returns.
/// @param title Borrowed window title copied into Canvas-owned storage; `NULL`
///        requests the backend default.
/// @param width Canvas width in logical pixels (scaled by HiDPI factor internally).
/// @param height Canvas height in logical pixels.
/// @return Owned Canvas handle, or `NULL` after invalid input, allocation, or
///         backend creation failure. Relevant failures trap first.
void *rt_canvas_new(rt_string title, int64_t width, int64_t height) {
    int32_t win_width = rt_canvas_dimension_to_i32(width, "Canvas.New: invalid width");
    int32_t win_height = rt_canvas_dimension_to_i32(height, "Canvas.New: invalid height");
    if (win_width <= 0 || win_height <= 0)
        return NULL;

    rt_canvas *canvas = (rt_canvas *)rt_obj_new_i64(RT_CANVAS_CLASS_ID, (int64_t)sizeof(rt_canvas));
    if (!canvas)
        return NULL;

    canvas->vptr = NULL;
    canvas->magic = RT_CANVAS_MAGIC;
    canvas->gfx_win = NULL;
    canvas->should_close = 0;
    canvas->title = NULL;
    canvas->title_len = 0;
    canvas->logical_width = (int64_t)win_width;
    canvas->logical_height = (int64_t)win_height;
    canvas->last_event.type = VGFX_EVENT_NONE;
    canvas->last_flip_us = 0;
    canvas->delta_time_ms = 0;
    canvas->dt_max_ms = 0;
    canvas->clip_enabled = 0;
    canvas->clip_x = 0;
    canvas->clip_y = 0;
    canvas->clip_w = 0;
    canvas->clip_h = 0;
    canvas->relative_mouse_applied = 0;
    canvas->window_state_synced = 0;
    canvas->applied_clip_enabled = 0;
    canvas->window_loan_active = 0;
    canvas->applied_coord_scale = 1.0f;
    canvas->applied_clip_x = 0;
    canvas->applied_clip_y = 0;
    canvas->applied_clip_w = 0;
    canvas->applied_clip_h = 0;
    rt_obj_set_finalizer(canvas, rt_canvas_finalize);

    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = win_width;
    params.height = win_height;
    if (title) {
        int64_t raw_title_len = rt_str_len(title);
        const char *cstr = rt_string_cstr(title);
        if (raw_title_len < 0 || !cstr || (uint64_t)raw_title_len > (uint64_t)SIZE_MAX - 1u) {
            if (rt_obj_release_check0(canvas))
                rt_obj_free(canvas);
            rt_trap("Canvas.New: invalid title");
            return NULL;
        }
        size_t title_len = (size_t)raw_title_len;
        canvas->title = (char *)malloc(title_len + 1);
        if (!canvas->title) {
            free(canvas->title);
            canvas->title = NULL;
            if (rt_obj_release_check0(canvas))
                rt_obj_free(canvas);
            rt_trap("Canvas.New: failed to allocate title buffer");
            return NULL;
        }
        memcpy(canvas->title, cstr, title_len);
        canvas->title[title_len] = '\0';
        canvas->title_len = title_len;
        params.title = canvas->title;
    }

    canvas->gfx_win = vgfx_create_window(&params);
    if (!canvas->gfx_win) {
        if (rt_obj_release_check0(canvas))
            rt_obj_free(canvas);
        rt_trap("Canvas.New: failed to create window (display server unavailable?)");
        return NULL;
    }

    // Enable coordinate scaling so Canvas apps draw in logical pixels while
    // the framebuffer may be HiDPI or fullscreen presentation sized.
    rt_canvas_resync_window_state(canvas);

    // Initialize keyboard input for this canvas
    rt_keyboard_set_canvas(canvas->gfx_win);

    // Initialize mouse input for this canvas
    rt_mouse_set_canvas(canvas->gfx_win);

    // Initialize gamepad input (no canvas reference needed)
    rt_pad_init();

    return canvas;
}

/// @brief Return 1 if `canvas_ptr` is a live canvas handle (magic sentinel check), 0 otherwise.
/// @param canvas_ptr Borrowed candidate handle.
/// @return `1` when class/layout/magic validation succeeds; otherwise `0`.
int8_t rt_canvas_is_handle(void *canvas_ptr) {
    return rt_canvas_checked(canvas_ptr) != NULL ? 1 : 0;
}

/// @brief Exclusively borrow the platform window behind a live 2D canvas.
/// @details Single-window adoption seam (Canvas3D.NewOnCanvas): the 3D
///          renderer attaches its GPU surface to this window instead of
///          creating a second one. The window stays owned by the 2D canvas, whose lifetime is
///          retained until `rt_canvas_return_window`. Concurrent adoption is rejected because
///          resize callbacks, input routing, pacing, and GPU presentation are per-window state.
/// @param canvas_ptr Candidate Canvas handle.
/// @return Borrowed vgfx window, or NULL for invalid, closed, or already-loaned canvases.
vgfx_window_t rt_canvas_borrow_window(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    int expected_loan_state = 0;
    if (!canvas || !rt_atomic_compare_exchange_i32(&canvas->window_loan_active,
                                                   &expected_loan_state,
                                                   1,
                                                   __ATOMIC_ACQ_REL,
                                                   __ATOMIC_ACQUIRE))
        return NULL;
    if (!canvas->gfx_win) {
        rt_atomic_store_i32(&canvas->window_loan_active, 2, __ATOMIC_RELEASE);
        return NULL;
    }
    rt_obj_retain_maybe(canvas);
    return canvas->gfx_win;
}

/// @brief Return an exclusive Canvas3D window loan and release the lender retain.
/// @details Presentation-state invalidation happens before the release because that release may
/// finalize a Canvas whose caller destroyed its own reference while Canvas3D was active.
/// @param canvas_ptr Canvas passed to a successful `rt_canvas_borrow_window` call.
void rt_canvas_return_window(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    int expected_loan_state = 1;
    if (!canvas || !rt_atomic_compare_exchange_i32(&canvas->window_loan_active,
                                                   &expected_loan_state,
                                                   0,
                                                   __ATOMIC_RELEASE,
                                                   __ATOMIC_RELAXED))
        return;
    canvas->window_state_synced = 0;
    if (rt_obj_release_check0(canvas))
        rt_obj_free(canvas);
}

/// @brief Mark a 2D canvas's cached window state stale.
/// @details Called when a borrowing Canvas3D returns the window: the 3D
///          canvas overwrote coord scale and presentation flags, so the 2D
///          canvas must re-push its own state on the next frame.
/// @param canvas_ptr Candidate Canvas handle; invalid handles are ignored.
void rt_canvas_mark_window_state_dirty(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return;
    canvas->window_state_synced = 0;
}

/// @brief Destroy a Canvas, releasing the window and associated resources.
/// @details Decrements the GC refcount. If the count reaches zero, the
///   finalizer frees the title string and destroys the ZannaGFX window.
/// @param canvas_ptr Owned Canvas reference to release; null/invalid handles
///        are ignored.
void rt_canvas_destroy(void *canvas_ptr) {
    if (!canvas_ptr)
        return;

    /* Validate before releasing — every other canvas entry point goes through
     * rt_canvas_checked, so a stale/wrong-type handle (or a second Destroy after
     * the finalizer zeroed the magic) is rejected here instead of underflowing the
     * refcount or freeing a non-canvas object. */
    if (!rt_canvas_checked(canvas_ptr))
        return;

    if (rt_obj_release_check0(canvas_ptr))
        rt_obj_free(canvas_ptr);
}

/// @brief Get the canvas width in logical pixels.
/// @details Returns the width as set during creation or after resize. On HiDPI
///   displays, the physical framebuffer may be larger; use GetScale() for the ratio.
/// @param canvas_ptr Canvas handle. Returns 0 if NULL or window not created.
/// @return Width in logical pixels, or 0 on error.
int64_t rt_canvas_width(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;

    rt_canvas_resync_window_state(canvas);

    if (canvas->logical_width > 0)
        return canvas->logical_width;

    int32_t width = 0;
    vgfx_get_size(canvas->gfx_win, &width, NULL);
    return (int64_t)width;
}

/// @brief Get the canvas height in logical pixels.
/// @details Resynchronizes cached native state before returning the cached
///          logical height, falling back to the backend size if needed.
/// @param canvas_ptr Canvas handle. Returns 0 if NULL or window not created.
/// @return Height in logical pixels, or 0 on error.
int64_t rt_canvas_height(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;

    rt_canvas_resync_window_state(canvas);

    if (canvas->logical_height > 0)
        return canvas->logical_height;

    int32_t height = 0;
    vgfx_get_size(canvas->gfx_win, NULL, &height);
    return (int64_t)height;
}

/// @brief Check whether the user has requested to close the canvas window.
/// @details Returns 1 after the OS close button is pressed or the window is
///   destroyed. Once set, this flag is permanent — the canvas cannot be reopened.
///   The game loop should check this each frame and exit when true.
/// @param canvas_ptr Canvas handle. Returns 1 (should close) if NULL.
/// @return 1 if the window should close, 0 if still open.
int64_t rt_canvas_should_close(void *canvas_ptr) {
    if (!canvas_ptr)
        return 1;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    return canvas ? canvas->should_close : 1;
}

/// @brief Present the back buffer to the screen and compute delta time.
/// @details Swaps the framebuffer (via vgfx_update), measures the time elapsed
///   since the previous Flip() call (stored as delta_time_ms), and checks if the
///   OS has requested window closure. This function must be called once per frame
///   after all drawing is complete.
///
///   If SetFps() was called, the ZannaGFX backend rate-limits Flip() to the
///   target frame rate — no additional sleep is needed.
///
///   Delta time is computed from monotonic microsecond timestamps and rounded
///   to the nearest millisecond. The first frame always reports dt=0.
/// @param canvas_ptr Canvas handle. NULL-safe (no-op).
void rt_canvas_flip(void *canvas_ptr) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    rt_canvas_resync_window_state(canvas);
    vgfx_update(canvas->gfx_win);

    /* Compute delta time between consecutive Flip() calls */
    int64_t now_us = rt_clock_ticks_us();
    if (canvas->last_flip_us > 0 && now_us > canvas->last_flip_us) {
        int64_t delta_us = now_us - canvas->last_flip_us;
        canvas->delta_time_ms = delta_us / 1000 + ((delta_us % 1000) >= 500 ? 1 : 0);
    } else {
        canvas->delta_time_ms = 0; /* first frame */
    }
    canvas->last_flip_us = now_us > 0 ? now_us : 0;

    /* Signal close to the application; caller checks canvas.should_close */
    if (vgfx_close_requested(canvas->gfx_win)) {
        rt_canvas_destroy_window(canvas);
        canvas->should_close = 1;
    }
}

/// @brief Get the time elapsed between the last two Flip() calls, in milliseconds.
/// @details If SetDTMax() was called with a positive value, the returned delta
///   time is clamped to the range [1, max]. This prevents physics explosions
///   after lag spikes or window drags. If dt_max is 0, the raw value is returned.
///   Returns 0 for the first frame (before a second Flip() has occurred).
/// @param canvas_ptr Canvas handle. Returns 0 if NULL.
/// @return Delta time in milliseconds, possibly clamped.
int64_t rt_canvas_get_delta_time(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return 0;
    int64_t dt = canvas->delta_time_ms;
    if (canvas->dt_max_ms > 0) {
        if (dt == 0)
            return 0;
        if (dt < 1)
            dt = 1;
        if (dt > canvas->dt_max_ms)
            dt = canvas->dt_max_ms;
    }
    return dt;
}

/// @brief Get DeltaTime in seconds for frame-rate-independent simulation code.
/// @param canvas_ptr Canvas handle. Returns 0.0 if NULL.
/// @return Delta time in seconds, using the same clamp as DeltaTime.
double rt_canvas_get_delta_time_sec(void *canvas_ptr) {
    return (double)rt_canvas_get_delta_time(canvas_ptr) / 1000.0;
}

/// @brief Fill the entire canvas with a solid color, erasing all previous drawing.
/// @details Typically called at the start of each frame before drawing game objects.
///   Accepts Canvas RGB or tagged Color.RGBA values; clear is opaque, so alpha is ignored.
/// @param canvas_ptr Borrowed Canvas handle; invalid/closed handles are ignored.
/// @param color Fill color.
void rt_canvas_clear(void *canvas_ptr, int64_t color) {
    if (!canvas_ptr)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        rt_canvas_resync_window_state(canvas);
        vgfx_cls(canvas->gfx_win,
                 (vgfx_color_t)((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu));
    }
}

/// @brief Poll for input events and update the keyboard, mouse, and gamepad state.
/// @details Processes all pending OS events (key presses, mouse movement, gamepad
///   input, window events). Must be called once per frame before reading input.
///   Internally calls rt_keyboard_begin_frame(), rt_mouse_begin_frame(), and
///   rt_pad_begin_frame() to reset per-frame input state, then dispatches all
///   queued events from the vgfx event queue.
///
///   Returns the type of the last event processed (0 if none). The return value
///   is rarely used directly — most games check Action.Pressed()/Held() instead.
///   A close event or event-pump failure destroys the native window, sets
///   ShouldClose, and still updates the action cache before returning.
/// @param canvas_ptr Borrowed Canvas handle.
/// @return Last event type processed, `VGFX_EVENT_CLOSE` for a queued close,
///         or `VGFX_EVENT_NONE` for no events, invalid input, or pump failure.
int64_t rt_canvas_poll(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;

    rt_canvas_resync_window_state(canvas);

    // Reset keyboard, mouse, and gamepad per-frame state at the start of polling
    rt_keyboard_begin_frame();
    rt_mouse_begin_frame();
    rt_pad_begin_frame();
    canvas->last_event.type = VGFX_EVENT_NONE;

    // Poll gamepads for state updates
    rt_pad_poll();

    // Pump the native event queue before draining translated events.
    if (!vgfx_pump_events(canvas->gfx_win)) {
        canvas->should_close = 1;
        rt_canvas_destroy_window(canvas);
        rt_action_update();
        return VGFX_EVENT_NONE;
    }

    /* Reconcile relative (raw) mouse mode with the platform window, mirroring
     * the Canvas3D poll: the input layer records the request, this poll owns
     * the window handle, so it applies the change and reports back whether
     * native raw deltas are available (else the warp-to-center path serves). */
    int8_t captured = rt_mouse_is_captured();
    int8_t relative_requested = (int8_t)(captured && rt_mouse_get_relative_mode());
    if (relative_requested != canvas->relative_mouse_applied) {
        int32_t native = vgfx_set_relative_mouse(canvas->gfx_win, relative_requested);
        rt_mouse_set_relative_native(relative_requested ? (int8_t)native : 0);
        canvas->relative_mouse_applied = relative_requested;
    }
    int8_t relative_native = (int8_t)(captured && rt_mouse_get_relative_native());

    if (captured && relative_native) {
        /* Native raw deltas: unbounded, unaccelerated, sub-pixel. */
        double rdx = 0.0;
        double rdy = 0.0;
        vgfx_get_relative_deltas(canvas->gfx_win, &rdx, &rdy);
        rt_mouse_force_delta_f(rdx, rdy);
    }

    int close_seen = 0;
    while (canvas->gfx_win && vgfx_poll_event(canvas->gfx_win, &canvas->last_event)) {
        if (canvas->last_event.type == VGFX_EVENT_CLOSE)
            close_seen = 1;

        // Forward keyboard events to keyboard module
        if (canvas->last_event.type == VGFX_EVENT_KEY_DOWN)
            rt_keyboard_on_vgfx_key_down((int64_t)canvas->last_event.data.key.key);
        else if (canvas->last_event.type == VGFX_EVENT_KEY_UP)
            rt_keyboard_on_vgfx_key_up((int64_t)canvas->last_event.data.key.key);
        else if (canvas->last_event.type == VGFX_EVENT_TEXT_INPUT)
            rt_keyboard_text_input((int32_t)canvas->last_event.data.text.codepoint);

        // Forward mouse events to mouse module (convert physical -> logical).
        // While captured, absolute move events are skipped — the delta comes
        // from native raw deltas or the warp-to-center fallback instead.
        if (!captured && canvas->last_event.type == VGFX_EVENT_MOUSE_MOVE) {
            rt_canvas_update_mouse_from_physical(
                canvas, canvas->last_event.data.mouse_move.x, canvas->last_event.data.mouse_move.y);
        } else if (canvas->last_event.type == VGFX_EVENT_MOUSE_DOWN) {
            if (!captured)
                rt_canvas_update_mouse_from_physical(canvas,
                                                     canvas->last_event.data.mouse_button.x,
                                                     canvas->last_event.data.mouse_button.y);
            rt_mouse_button_down((int64_t)canvas->last_event.data.mouse_button.button);
        } else if (canvas->last_event.type == VGFX_EVENT_MOUSE_UP) {
            if (!captured)
                rt_canvas_update_mouse_from_physical(canvas,
                                                     canvas->last_event.data.mouse_button.x,
                                                     canvas->last_event.data.mouse_button.y);
            rt_mouse_button_up((int64_t)canvas->last_event.data.mouse_button.button);
        } else if (canvas->last_event.type == VGFX_EVENT_SCROLL) {
            if (!captured)
                rt_canvas_update_mouse_from_physical(
                    canvas, canvas->last_event.data.scroll.x, canvas->last_event.data.scroll.y);
            rt_mouse_update_wheel((double)canvas->last_event.data.scroll.delta_x,
                                  (double)canvas->last_event.data.scroll.delta_y);
        } else if (canvas->last_event.type == VGFX_EVENT_RESIZE) {
            /* Keep the cached logical size in sync with OS-driven resizes (user drag,
             * Maximize/Restore); otherwise Canvas.Width/Height report the creation
             * size forever while draw/clip use the live window size. */
            if (canvas->last_event.data.resize.logical_width > 0)
                canvas->logical_width = (int64_t)canvas->last_event.data.resize.logical_width;
            if (canvas->last_event.data.resize.logical_height > 0)
                canvas->logical_height = (int64_t)canvas->last_event.data.resize.logical_height;
            canvas->window_state_synced = 0;
        }
    }

    if (close_seen) {
        canvas->should_close = 1;
        rt_canvas_destroy_window(canvas);
        rt_action_update();
        return VGFX_EVENT_CLOSE;
    }

    // Finish with the live cursor position so queued historical move events
    // cannot leave the frame using stale coordinates.
    int32_t mx = 0, my = 0;
    vgfx_mouse_pos(canvas->gfx_win, &mx, &my);
    if (captured && !relative_native) {
        /* Warp-to-center fallback: the frame's delta is the offset from the
         * window center the cursor was warped to at the end of the last poll. */
        int32_t cw = 0, ch = 0;
        vgfx_get_size(canvas->gfx_win, &cw, &ch);
        int32_t cx = cw / 2, cy = ch / 2;
        rt_mouse_force_delta((int64_t)mx - (int64_t)cx, (int64_t)my - (int64_t)cy);
    } else if (!captured) {
        rt_mouse_update_pos((int64_t)mx, (int64_t)my);

        // Recompute the absolute delta now that this frame's motion has been
        // applied, so Mouse.DeltaX/Y describe the same frame as Mouse.X/Y.
        rt_mouse_finalize_frame();
    }

    // Update action mapping state AFTER events are processed so that
    // Action.Pressed/Held/Released reflect this frame's input.
    rt_action_update();

    /* Warp cursor to center for next frame (only for the captured fallback
     * path — native relative mode never needs warping). */
    if (captured && !relative_native && canvas->gfx_win) {
        int32_t cw = 0, ch = 0;
        vgfx_get_size(canvas->gfx_win, &cw, &ch);
        vgfx_warp_cursor(canvas->gfx_win, cw / 2, ch / 2);
    }

    /* Returns the type of the LAST event processed this frame (not a boolean).
     * Close detection is via Canvas.ShouldClose, not via this return value. */
    return (int64_t)canvas->last_event.type;
}

/// @brief Check if a key is currently held down (raw vgfx key query).
/// @details This is a low-level function; most games use Action.Held() instead.
///   Queries the ZannaGFX backend directly for the current key state.
/// @param canvas_ptr Canvas handle. Returns 0 if NULL.
/// @param key ZannaGFX key code (from vgfx.h constants).
/// @return 1 if the key is currently pressed, 0 if released or invalid.
int64_t rt_canvas_key_held(void *canvas_ptr, int64_t key) {
    if (!canvas_ptr)
        return 0;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;

    if (!rtg_i64_fits_i32(key))
        return 0;
    return (int64_t)vgfx_key_down(canvas->gfx_win, (vgfx_key_t)(int32_t)key);
}

//=============================================================================
// Canvas Window Management
//=============================================================================

/// @brief Set the clipping rectangle for all subsequent drawing operations.
/// @details Only pixels within the clip rect will be drawn. Useful for HUD panels,
///   minimap viewports, or any region-restricted rendering. Call ClearClipRect()
///   to restore full-canvas drawing. Nonpositive dimensions are stored as zero.
/// @param canvas_ptr Canvas handle. NULL-safe.
/// @param x Left edge of clip region (logical pixels).
/// @param y Top edge of clip region.
/// @param w Width of clip region.
/// @param h Height of clip region.
void rt_canvas_set_clip_rect(void *canvas_ptr, int64_t x, int64_t y, int64_t w, int64_t h) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        canvas->clip_enabled = 1;
        canvas->clip_x = x;
        canvas->clip_y = y;
        canvas->clip_w = w > 0 ? w : 0;
        canvas->clip_h = h > 0 ? h : 0;
        rt_canvas_resync_window_state(canvas);
    }
}

/// @brief Remove the clipping rectangle, restoring full-canvas drawing.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_clear_clip_rect(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        canvas->clip_enabled = 0;
        canvas->clip_x = 0;
        canvas->clip_y = 0;
        canvas->clip_w = 0;
        canvas->clip_h = 0;
        rt_canvas_resync_window_state(canvas);
    }
}

/// @brief Change the window title bar text.
/// @details Updates both the OS window title and the internal cached copy
///   (used by GetTitle). The old cached title is freed only after the new copy
///   is allocated; invalid input or allocation failure preserves it.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param title Borrowed new title string; `NULL` is ignored.
void rt_canvas_set_title(void *canvas_ptr, rt_string title) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win && title) {
        int64_t raw_title_len = rt_str_len(title);
        const char *cstr = rt_string_cstr(title);
        if (raw_title_len < 0 || !cstr || (uint64_t)raw_title_len > (uint64_t)SIZE_MAX - 1u)
            return;
        size_t title_len = (size_t)raw_title_len;
        char *new_title = (char *)malloc(title_len + 1);
        if (!new_title)
            return;
        memcpy(new_title, cstr, title_len);
        new_title[title_len] = '\0';
        vgfx_set_title(canvas->gfx_win, new_title);
        free(canvas->title);
        canvas->title = new_title;
        canvas->title_len = title_len;
    }
}

/// @brief Get the current window title.
/// @param canvas_ptr Borrowed Canvas handle.
/// @return Newly owned copy of the cached title, an owned empty string for
///         invalid/untitled canvases, or `NULL` on string allocation failure.
rt_string rt_canvas_get_title(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->title)
        return rt_string_from_bytes(canvas->title, canvas->title_len);
    return rt_string_from_bytes("", 0);
}

/// @brief Resize the canvas window to new dimensions.
/// @details Dimensions must be positive and fit int32; invalid dimensions trap
///          and leave the live window unchanged.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param width New width in logical pixels.
/// @param height New height in logical pixels.
void rt_canvas_resize(void *canvas_ptr, int64_t width, int64_t height) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        int32_t win_width = rt_canvas_dimension_to_i32(width, "Canvas.Resize: invalid width");
        int32_t win_height = rt_canvas_dimension_to_i32(height, "Canvas.Resize: invalid height");
        if (win_width <= 0 || win_height <= 0)
            return;
        canvas->logical_width = (int64_t)win_width;
        canvas->logical_height = (int64_t)win_height;
        vgfx_set_window_size(canvas->gfx_win, win_width, win_height);
        canvas->window_state_synced = 0;
        rt_canvas_resync_window_state(canvas);
    }
}

/// @brief Programmatically close the canvas window.
/// @details Destroys the ZannaGFX window and sets should_close=1. After this
///   call, all drawing operations become no-ops and ShouldClose returns true.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_close(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas) {
        rt_canvas_destroy_window(canvas);
        if (!canvas->gfx_win)
            canvas->should_close = 1;
    }
}

/// @brief Capture the current canvas contents as a Pixels object.
/// @details Creates a new Pixels object containing a copy of the framebuffer.
///   The returned object is GC-managed and can be saved to BMP/PNG or composited.
/// @param canvas_ptr Borrowed live Canvas handle.
/// @return Owned Pixels object with the canvas contents, or `NULL` on failure.
void *rt_canvas_screenshot(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return NULL;
    if (!canvas->gfx_win)
        return NULL;

    int32_t w, h;
    if (vgfx_get_size(canvas->gfx_win, &w, &h) == 0)
        return NULL;

    return rt_canvas_copy_rect(canvas_ptr, 0, 0, w, h);
}

/// @brief Switch the canvas window to fullscreen mode.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_fullscreen(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        vgfx_set_fullscreen(canvas->gfx_win, 1);
        canvas->window_state_synced = 0;
        rt_canvas_resync_window_state(canvas);
    }
}

/// @brief Switch the canvas window back to windowed mode from fullscreen.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_windowed(void *canvas_ptr) {
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        vgfx_set_fullscreen(canvas->gfx_win, 0);
        canvas->window_state_synced = 0;
        rt_canvas_resync_window_state(canvas);
    }
}

/// @brief Set the target frame rate for the canvas.
/// @details The ZannaGFX backend rate-limits Flip() to this target. Pass -1 to
///   disable rate limiting (unlimited FPS). Values below -1 become -1, and
///   larger values saturate to the backend int32 range. Default is unlimited.
/// @param canvas_ptr Canvas handle. NULL-safe.
/// @param fps Target frames per second (-1 for unlimited).
void rt_canvas_set_fps(void *canvas_ptr, int64_t fps) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win) {
        if (fps < -1)
            fps = -1;
        vgfx_set_fps(canvas->gfx_win, rtg_clamp_i64_to_i32(fps));
    }
}

/// @brief Get the configured target frame rate.
/// @param canvas_ptr Canvas handle. Returns -1 if NULL.
/// @return Target FPS, or -1 if unlimited or error.
int64_t rt_canvas_get_fps(void *canvas_ptr) {
    if (!canvas_ptr)
        return -1;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return -1;
    return (int64_t)vgfx_get_fps(canvas->gfx_win);
}

/// @brief Set the maximum delta time clamp in milliseconds.
/// @details When set to a positive value, get_delta_time() clamps the returned
///   DeltaTime to [1, max_ms]. This prevents physics explosions after lag spikes.
///   Set to 0 to disable clamping. A typical game value is 50ms (20 FPS equivalent).
/// @param canvas_ptr Canvas handle. NULL-safe.
/// @param max_ms Maximum delta time in ms. 0 disables clamping. Negative treated as 0.
void rt_canvas_set_dt_max(void *canvas_ptr, int64_t max_ms) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas)
        return;
    canvas->dt_max_ms = max_ms > 0 ? max_ms : 0;
}

/// @brief Combined Poll + ShouldClose check for simplified game loops.
/// @details Calls Poll() to process input events, then returns 0 if the window
///   should close, or 1 if the frame should continue. Replaces the common pattern:
///     canvas.Poll();
///     if canvas.ShouldClose { break; }
///   with:
///     while canvas.BeginFrame() != 0 { ... }
/// @param canvas_ptr Canvas handle. Returns 0 (stop) if NULL.
/// @return 1 to continue the frame, 0 to stop (window closing).
int64_t rt_canvas_begin_frame(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;
    rt_canvas_poll(canvas_ptr);
    return rt_canvas_should_close(canvas_ptr) ? 0 : 1;
}

/// @brief Get the HiDPI scale factor for the canvas display.
/// @details The canvas draws in logical pixels while the framebuffer may be
///   scale-factor times larger.
/// @param canvas_ptr Canvas handle. Returns 1.0 if NULL.
/// @return Positive backend scale factor, or `1.0` for invalid/closed canvases.
double rt_canvas_get_scale(void *canvas_ptr) {
    if (!canvas_ptr)
        return 1.0;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 1.0;
    return (double)vgfx_window_get_scale(canvas->gfx_win);
}

/// @brief Get the window X position as a runtime-callable scalar.
/// @param canvas_ptr Borrowed Canvas handle.
/// @return Desktop X coordinate, or `0` for invalid/closed canvases.
int64_t rt_canvas_get_window_x(void *canvas_ptr) {
    int64_t x = 0;
    rt_canvas_get_position(canvas_ptr, &x, NULL);
    return x;
}

/// @brief Get the window Y position as a runtime-callable scalar.
/// @param canvas_ptr Borrowed Canvas handle.
/// @return Desktop Y coordinate, or `0` for invalid/closed canvases.
int64_t rt_canvas_get_window_y(void *canvas_ptr) {
    int64_t y = 0;
    rt_canvas_get_position(canvas_ptr, NULL, &y);
    return y;
}

/// @brief Get the window position on screen in desktop coordinates.
/// @details Invalid/closed canvases leave provided outputs unchanged.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param out_x Optional output receiving X position.
/// @param out_y Optional output receiving Y position.
void rt_canvas_get_position(void *canvas_ptr, int64_t *out_x, int64_t *out_y) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    int32_t x = 0, y = 0;
    vgfx_get_position(canvas->gfx_win, &x, &y);
    if (out_x)
        *out_x = (int64_t)x;
    if (out_y)
        *out_y = (int64_t)y;
}

/// @brief Move the window to a specific position on the desktop.
/// @details Coordinates outside the backend int32 range are saturated.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param x Desktop X coordinate for the window's top-left corner.
/// @param y Desktop Y coordinate.
void rt_canvas_set_position(void *canvas_ptr, int64_t x, int64_t y) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win)
        vgfx_set_position(canvas->gfx_win, rtg_clamp_i64_to_i32(x), rtg_clamp_i64_to_i32(y));
}

/// @brief Check if the window is currently maximized.
/// @param canvas_ptr Canvas handle. Returns 0 if NULL.
/// @return 1 if maximized, 0 if not.
int8_t rt_canvas_is_maximized(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;
    return (int8_t)vgfx_is_maximized(canvas->gfx_win);
}

/// @brief Maximize the window to fill the screen (not fullscreen — keeps title bar).
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_maximize(void *canvas_ptr) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win)
        vgfx_maximize(canvas->gfx_win);
}

/// @brief Check if the window is currently minimized (iconified).
/// @param canvas_ptr Canvas handle. Returns 0 if NULL.
/// @return 1 if minimized, 0 if not.
int8_t rt_canvas_is_minimized(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;
    return (int8_t)vgfx_is_minimized(canvas->gfx_win);
}

/// @brief Minimize the window to the taskbar/dock.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_minimize(void *canvas_ptr) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win)
        vgfx_minimize(canvas->gfx_win);
}

/// @brief Restore the window from minimized or maximized state to its previous size.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_restore(void *canvas_ptr) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win)
        vgfx_restore(canvas->gfx_win);
}

/// @brief Check if the window currently has keyboard/mouse focus.
/// @param canvas_ptr Canvas handle. Returns 0 if NULL.
/// @return 1 if focused, 0 if not.
int8_t rt_canvas_is_focused(void *canvas_ptr) {
    if (!canvas_ptr)
        return 0;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return 0;
    return (int8_t)vgfx_is_focused(canvas->gfx_win);
}

/// @brief Request the OS to give keyboard/mouse focus to this window.
/// @param canvas_ptr Canvas handle. NULL-safe.
void rt_canvas_focus(void *canvas_ptr) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win)
        vgfx_focus(canvas->gfx_win);
}

/// @brief Block or unblock the OS close button.
/// @details When prevent is non-zero, clicking the window's close button does NOT
///   set ShouldClose. The app must call PreventClose(0) before the user can close.
///   Useful for "unsaved changes" prompts.
/// @param canvas_ptr Canvas handle. NULL-safe.
/// @param prevent Non-zero to block close, 0 to allow.
void rt_canvas_prevent_close(void *canvas_ptr, int64_t prevent) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (canvas && canvas->gfx_win)
        vgfx_set_prevent_close(canvas->gfx_win, (int32_t)(prevent != 0));
}

/// @brief Get the resolution of the monitor containing this window.
/// @details Invalid/closed canvases leave provided outputs unchanged.
/// @param canvas_ptr Borrowed Canvas handle.
/// @param out_w Optional output receiving monitor width.
/// @param out_h Optional output receiving monitor height.
void rt_canvas_get_monitor_size(void *canvas_ptr, int64_t *out_w, int64_t *out_h) {
    if (!canvas_ptr)
        return;
    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    int32_t w = 0, h = 0;
    vgfx_get_monitor_size(canvas->gfx_win, &w, &h);
    if (out_w)
        *out_w = (int64_t)w;
    if (out_h)
        *out_h = (int64_t)h;
}

/// @brief Get the current monitor width as a runtime-callable scalar.
/// @param canvas_ptr Borrowed Canvas handle.
/// @return Monitor width in pixels, or `0` for invalid/closed canvases.
int64_t rt_canvas_get_monitor_width(void *canvas_ptr) {
    int64_t w = 0;
    rt_canvas_get_monitor_size(canvas_ptr, &w, NULL);
    return w;
}

/// @brief Get the current monitor height as a runtime-callable scalar.
/// @param canvas_ptr Borrowed Canvas handle.
/// @return Monitor height in pixels, or `0` for invalid/closed canvases.
int64_t rt_canvas_get_monitor_height(void *canvas_ptr) {
    int64_t h = 0;
    rt_canvas_get_monitor_size(canvas_ptr, NULL, &h);
    return h;
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
