//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaGFX Core Implementation
//
// Platform-agnostic implementation of the ZannaGFX API.  Provides window
// lifecycle management, event queue operations, framebuffer operations,
// and input polling.  Platform-specific functionality is delegated to the
// platform backend via function pointers defined in vgfx_internal.h.
//
// Key Design Decisions:
//   - Thread-Local Error Storage: Errors are thread-local (C11 _Thread_local)
//     so concurrent windows can have independent error states.
//   - Event Queue: Uses a synchronized ring buffer with FIFO eviction policy
//     that prioritizes CLOSE events.
//   - Aligned Framebuffer: Allocated with VGFX_FRAMEBUFFER_ALIGNMENT for
//     cache performance and potential SIMD optimizations.
//   - Integer-Only Math: All coordinates and dimensions use int32_t for
//     deterministic, portable behavior.
//   - FPS Limiting: Deadline-based scheduling that resyncs if falling behind.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Core implementation of the ZannaGFX API (platform-agnostic).
/// @details Implements window management, event handling, drawing operations,
///          and input polling.  Delegates OS-specific tasks to the platform
///          backend (vgfx_platform_*.c).

#include "vgfx.h"
#include "vgfx_embed_channel.h"
#include "vgfx_internal.h"

/* Embedded-play tap (ADR 0225) — definitions precede vgfx_pump_events. */
static void vgfx_embed_present_tap(vgfx_window_t window);
static void vgfx_embed_input_tap(vgfx_window_t window);
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Thread-Local Error State
//===----------------------------------------------------------------------===//
// Errors are stored per-thread so concurrent windows can have independent
// error states.  Uses C11 _Thread_local on conforming compilers, falls back
// to platform-specific TLS on Windows, and global storage as last resort.
//===----------------------------------------------------------------------===//

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/// @brief C11 thread-local error message string.
_Thread_local static const char *g_last_error_str = NULL;
/// @brief C11 thread-local error code.
_Thread_local static vgfx_error_t g_last_error_code = VGFX_ERR_NONE;
#elif defined(_WIN32)
/// @brief Windows TLS error message string.
__declspec(thread) static const char *g_last_error_str = NULL;
/// @brief Windows TLS error code.
__declspec(thread) static vgfx_error_t g_last_error_code = VGFX_ERR_NONE;
#elif defined(__GNUC__) || defined(__clang__)
/// @brief GCC/Clang TLS error message string.
__thread static const char *g_last_error_str = NULL;
/// @brief GCC/Clang TLS error code.
__thread static vgfx_error_t g_last_error_code = VGFX_ERR_NONE;
#else
/// @brief Fallback global error message (not thread-safe).
static const char *g_last_error_str = NULL;
/// @brief Fallback global error code (not thread-safe).
static vgfx_error_t g_last_error_code = VGFX_ERR_NONE;
#endif

//===----------------------------------------------------------------------===//
// Global State
//===----------------------------------------------------------------------===//

/// @brief Global default FPS applied when window params specify fps == 0.
static int32_t g_default_fps = VGFX_DEFAULT_FPS;

/// @brief Optional user-provided logging callback for error messages.
static vgfx_log_fn g_log_callback = NULL;

/// @brief Protects process-wide configuration globals.
static vgfx_atomic_flag_t g_config_lock;

//===----------------------------------------------------------------------===//
// Internal Helper Functions
//===----------------------------------------------------------------------===//

/// @brief Acquire the process-wide configuration lock.
/// @details Used for small global settings such as the default FPS and log
///          callback. The lock is intentionally independent from per-window
///          event locks so configuration updates do not block event queues.
static void vgfx_config_lock(void) {
    while (vgfx_atomic_flag_test_and_set(&g_config_lock))
        vgfx_internal_event_wait();
}

/// @brief Release the process-wide configuration lock.
static void vgfx_config_unlock(void) {
    vgfx_atomic_flag_clear(&g_config_lock);
}

/// @brief Read the default FPS under the global configuration lock.
/// @return Current process-wide default FPS.
static int32_t vgfx_read_default_fps(void) {
    vgfx_config_lock();
    int32_t fps = g_default_fps;
    vgfx_config_unlock();
    return fps;
}

/// @brief Read the optional log callback under the global configuration lock.
/// @return Current log callback, or NULL if logging callback forwarding is disabled.
static vgfx_log_fn vgfx_read_log_callback(void) {
    vgfx_config_lock();
    vgfx_log_fn fn = g_log_callback;
    vgfx_config_unlock();
    return fn;
}

/// @brief Return whether internal errors should also be mirrored to stderr.
/// @details ZannaGFX is often embedded in tools that manage their own diagnostic
///          output.  Stderr mirroring is therefore opt-in via the
///          `ZANNA_GFX_STDERR` environment variable; the user log callback still
///          receives errors regardless of this setting.
/// @return 1 when stderr mirroring is enabled, 0 otherwise.
static int vgfx_should_log_to_stderr(void) {
    const char *value = getenv("ZANNA_GFX_STDERR");
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

/// @brief Set the thread-local error state and invoke logging.
/// @details Stores the error code/message in TLS and calls the user-provided
///          log callback (if any).  This function
///          is called by both the core library and platform backends when
///          an error occurs.
///
/// @param code Error code (enum vgfx_error_t)
/// @param msg  Descriptive error message (UTF-8, may be NULL)
///
/// @post g_last_error_code == code
/// @post g_last_error_str == msg
/// @post Message printed to stderr only when ZANNA_GFX_STDERR is enabled.
/// @post Log callback invoked (if set and msg != NULL)
void vgfx_internal_set_error(vgfx_error_t code, const char *msg) {
    g_last_error_code = code;
    g_last_error_str = msg;

    /* Print to stderr only when explicitly requested by the embedding process. */
    if (msg && vgfx_should_log_to_stderr()) {
        fprintf(stderr, "vgfx: %s\n", msg);
    }

    /* Call logging callback if set. Copy it first so invocation is outside
     * the global config lock and user logging cannot deadlock configuration. */
    vgfx_log_fn log_callback = vgfx_read_log_callback();
    if (log_callback && msg) {
        log_callback(msg);
    }
}

/// @brief Clamp an integer value to the range [min, max].
/// @details Returns the value unchanged if within range, otherwise returns
///          the nearest boundary.  Used for sanitizing FPS values.
///
/// @param value Input value to clamp
/// @param min   Minimum allowed value
/// @param max   Maximum allowed value
/// @return value clamped to [min, max]
static inline int32_t clamp_int(int32_t value, int32_t min, int32_t max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

/// @brief Build a host-endian word whose object representation is RGBA bytes.
/// @details The framebuffer format is byte-oriented RGBA.  Constructing the word
///          through `memcpy` preserves the desired byte order on every host
///          endian while still allowing fast 32-bit stores into aligned pixels.
/// @param r Red channel byte.
/// @param g Green channel byte.
/// @param b Blue channel byte.
/// @param a Alpha channel byte.
/// @return A uint32_t whose memory bytes are exactly r,g,b,a.
static uint32_t make_rgba_word(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t bytes[4] = {r, g, b, a};
    uint32_t word = 0;
    memcpy(&word, bytes, sizeof(word));
    return word;
}

/// @brief Fill a contiguous RGBA pixel span with one color.
/// @details Writes through `memcpy` so the byte-oriented framebuffer contract
///          does not rely on the destination being suitably aligned for
///          uint32_t stores or on aliasing through a wider integer type.
/// @param pixels First RGBA byte of the span.
/// @param pixel_count Number of pixels to fill.
/// @param r Red channel byte.
/// @param g Green channel byte.
/// @param b Blue channel byte.
/// @param a Alpha channel byte.
static void fill_rgba_pixels(
    uint8_t *pixels, size_t pixel_count, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!pixels || pixel_count == 0)
        return;
    uint32_t word = make_rgba_word(r, g, b, a);
    memcpy(pixels, &word, sizeof(word));
    size_t filled = 1;
    while (filled < pixel_count) {
        size_t copy = filled;
        if (copy > pixel_count - filled)
            copy = pixel_count - filled;
        memcpy(pixels + filled * 4u, pixels, copy * 4u);
        filled += copy;
    }
}

/// @brief Clear a raw framebuffer byte range to opaque black RGBA pixels.
/// @param pixels First framebuffer byte.
/// @param size Size in bytes; trailing non-pixel bytes are ignored defensively.
static void clear_framebuffer_rgba(uint8_t *pixels, size_t size) {
    fill_rgba_pixels(pixels, size / 4u, 0, 0, 0, 0xFF);
}

/// @brief Retain a replaced framebuffer allocation until window destruction.
/// @details The public framebuffer API exposes raw pixel pointers. Keeping old
///          allocations alive after resize prevents stale descriptors from
///          becoming immediate use-after-free hazards; generation checks still
///          remain the caller's way to detect and stop using stale storage.
/// @param win Window that owns the retired allocation list.
/// @param pixels Previous aligned framebuffer pointer, or NULL.
static void vgfx_retire_framebuffer(struct vgfx_window *win, uint8_t *pixels) {
    if (!win || !pixels)
        return;

    vgfx_retired_framebuffer_t *node =
        (vgfx_retired_framebuffer_t *)malloc(sizeof(vgfx_retired_framebuffer_t));
    if (!node) {
        vgfx_platform_aligned_free(pixels);
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to retain retired framebuffer");
        return;
    }

    node->pixels = pixels;
    node->next = win->retired_framebuffers;
    win->retired_framebuffers = node;
}

/// @brief Free every framebuffer allocation retired by prior resizes.
/// @param win Window whose retired framebuffer list should be released.
static void vgfx_free_retired_framebuffers(struct vgfx_window *win) {
    if (!win)
        return;

    vgfx_retired_framebuffer_t *node = win->retired_framebuffers;
    win->retired_framebuffers = NULL;
    while (node) {
        vgfx_retired_framebuffer_t *next = node->next;
        vgfx_platform_aligned_free(node->pixels);
        free(node);
        node = next;
    }
}

/// @brief Compute storage required for a tightly packed RGBA framebuffer.
/// @param width Positive physical width in pixels.
/// @param height Positive physical height in pixels.
/// @param out_size Receives `width * height * 4` on success.
/// @return 1 when the byte count is representable in size_t; otherwise 0.
static int framebuffer_size_bytes(int32_t width, int32_t height, size_t *out_size) {
    if (!out_size || width <= 0 || height <= 0)
        return 0;
    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / h)
        return 0;
    size_t pixels = w * h;
    if (pixels > SIZE_MAX / 4u)
        return 0;
    *out_size = pixels * 4u;
    return 1;
}

/// @copydoc vgfx_internal_resize_framebuffer
int vgfx_internal_resize_framebuffer(struct vgfx_window *win, int32_t width, int32_t height) {
    if (!win || width <= 0 || height <= 0) {
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM, "Invalid framebuffer resize dimensions");
        return 0;
    }
    if (width > VGFX_MAX_WIDTH || height > VGFX_MAX_HEIGHT) {
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM,
                                "Framebuffer resize dimensions exceed maximum");
        return 0;
    }
    if (win->width == width && win->height == height)
        return 1;

    size_t fb_size = 0;
    if (!framebuffer_size_bytes(width, height, &fb_size)) {
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM, "Framebuffer resize size overflow");
        return 0;
    }
    uint8_t *new_pixels =
        (uint8_t *)vgfx_platform_aligned_alloc(VGFX_FRAMEBUFFER_ALIGNMENT, fb_size);
    if (!new_pixels) {
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate resized framebuffer");
        return 0;
    }

    clear_framebuffer_rgba(new_pixels, fb_size);

    vgfx_internal_event_lock(win);
    uint8_t *old_pixels = win->pixels;
    win->pixels = new_pixels;
    win->width = width;
    win->height = height;
    win->stride = width * 4;
    win->framebuffer_generation++;

    if (win->clip_enabled) {
        if (win->clip_x >= width || win->clip_y >= height) {
            win->clip_enabled = 0;
        } else {
            int32_t max_w = width - win->clip_x;
            int32_t max_h = height - win->clip_y;
            if (win->clip_w > max_w)
                win->clip_w = max_w;
            if (win->clip_h > max_h)
                win->clip_h = max_h;
            if (win->clip_w <= 0 || win->clip_h <= 0)
                win->clip_enabled = 0;
        }
    }
    vgfx_internal_event_unlock(win);

    vgfx_retire_framebuffer(win, old_pixels);

    return 1;
}

/// @copydoc vgfx_internal_clear_input_state
void vgfx_internal_clear_input_state(struct vgfx_window *win) {
    if (!win)
        return;
    vgfx_internal_event_lock(win);
    memset(win->key_state, 0, sizeof(win->key_state));
    memset(win->mouse_button_state, 0, sizeof(win->mouse_button_state));
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_set_key_state
void vgfx_internal_set_key_state(struct vgfx_window *win, int32_t key, int32_t down) {
    if (!win || key <= (int32_t)VGFX_KEY_UNKNOWN || key >= 512)
        return;
    vgfx_internal_event_lock(win);
    win->key_state[key] = down ? 1u : 0u;
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_set_mouse_button_state
void vgfx_internal_set_mouse_button_state(struct vgfx_window *win, int32_t button, int32_t down) {
    if (!win || button < 0 || button >= 8)
        return;
    vgfx_internal_event_lock(win);
    win->mouse_button_state[button] = down ? 1u : 0u;
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_set_mouse_position
void vgfx_internal_set_mouse_position(struct vgfx_window *win, int32_t x, int32_t y) {
    if (!win)
        return;
    vgfx_internal_event_lock(win);
    win->mouse_x = x;
    win->mouse_y = y;
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_add_relative_delta
void vgfx_internal_add_relative_delta(struct vgfx_window *win, double dx, double dy) {
    if (!win)
        return;
    vgfx_internal_event_lock(win);
    win->relative_dx_accum += dx;
    win->relative_dy_accum += dy;
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_set_close_requested
void vgfx_internal_set_close_requested(struct vgfx_window *win, int32_t requested) {
    if (!win)
        return;
    vgfx_internal_event_lock(win);
    win->close_requested = requested ? 1 : 0;
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_set_focus_state
void vgfx_internal_set_focus_state(struct vgfx_window *win, int32_t focused) {
    if (!win)
        return;
    vgfx_internal_event_lock(win);
    win->is_focused = focused ? 1 : 0;
    vgfx_internal_event_unlock(win);
}

/// @copydoc vgfx_internal_set_prevent_close
void vgfx_internal_set_prevent_close(struct vgfx_window *win, int32_t prevent) {
    if (!win)
        return;
    vgfx_internal_event_lock(win);
    win->prevent_close = prevent ? 1 : 0;
    vgfx_internal_event_unlock(win);
}

//===----------------------------------------------------------------------===//
// Event Queue Implementation (Synchronized Ring Buffer)
//===----------------------------------------------------------------------===//
// Protected so platform callbacks and application polling do not race. Uses
// FIFO eviction when full, with special handling for CLOSE events.
//===----------------------------------------------------------------------===//

/// @brief Initialize a bounded value-type event for one native IME lifecycle transition.
/// @details See the internal header for the full ownership and index-unit contract. When the
///          native UTF-8 payload exceeds inline capacity, the copied prefix ends before the first
///          codepoint that would cross the bound and the event exposes `truncated = 1`.
/// @param event Destination event initialized from scratch.
/// @param type Composition lifecycle discriminator.
/// @param time_ms Native event timestamp in milliseconds.
/// @param text UTF-8 payload, or NULL only when @p text_length is zero.
/// @param text_length Payload length in bytes.
/// @param selection_start Preedit selection start in Unicode codepoints.
/// @param selection_length Preedit selection length in Unicode codepoints.
/// @param replacement_start Committed-text replacement start or -1 sentinel.
/// @param replacement_length Replacement codepoint count or -1 with the sentinel start.
/// @param modifiers Active VGFX_MOD_* mask.
/// @return 1 when a supported event was initialized; otherwise 0.
int vgfx_internal_init_composition_event(vgfx_event_t *event,
                                         vgfx_event_type_t type,
                                         int64_t time_ms,
                                         const char *text,
                                         size_t text_length,
                                         int32_t selection_start,
                                         int32_t selection_length,
                                         int32_t replacement_start,
                                         int32_t replacement_length,
                                         int modifiers) {
    if (!event)
        return 0;
    if (type != VGFX_EVENT_COMPOSITION_START && type != VGFX_EVENT_COMPOSITION_UPDATE &&
        type != VGFX_EVENT_COMPOSITION_COMMIT && type != VGFX_EVENT_COMPOSITION_CANCEL)
        return 0;
    if (!text && text_length != 0)
        return 0;

    memset(event, 0, sizeof(*event));
    event->type = type;
    event->time_ms = time_ms;

    size_t copy_length = text_length;
    const size_t maximum = (size_t)VGFX_COMPOSITION_TEXT_CAPACITY - 1u;
    if (copy_length > maximum) {
        copy_length = maximum;
        while (copy_length > 0 &&
               (((const unsigned char *)text)[copy_length] & UINT8_C(0xC0)) == UINT8_C(0x80)) {
            copy_length--;
        }
        event->data.composition.truncated = 1;
    }
    if (copy_length > 0)
        memcpy(event->data.composition.text, text, copy_length);
    event->data.composition.text[copy_length] = '\0';
    event->data.composition.text_length = (uint32_t)copy_length;
    event->data.composition.selection_start = selection_start < 0 ? 0 : selection_start;
    event->data.composition.selection_length = selection_length < 0 ? 0 : selection_length;
    event->data.composition.replacement_start = replacement_start;
    event->data.composition.replacement_length = replacement_length;
    event->data.composition.modifiers = modifiers;
    return 1;
}

/// @brief Yield during event queue lock contention.
/// @details Uses the native scheduler yield primitive where available.  This
///          keeps the queue's critical section lightweight while preventing
///          producer/consumer contention from becoming a pure busy-wait.
void vgfx_internal_event_wait(void) {
    vgfx_platform_yield();
}

/// @brief Test whether an event must survive queue pressure.
/// @details Release-like state transitions repair sticky input state and close
///          events carry the destruction request.  Dropping these events can
///          leave callers with a permanently pressed key/button or a missed close
///          notification.
/// @param type Event type to classify.
/// @return Non-zero for close, release/focus-loss, and IME lifecycle boundary events.
static int vgfx_event_is_release_state_event(vgfx_event_type_t type) {
    return type == VGFX_EVENT_CLOSE || type == VGFX_EVENT_KEY_UP || type == VGFX_EVENT_MOUSE_UP ||
           type == VGFX_EVENT_FOCUS_LOST || type == VGFX_EVENT_COMPOSITION_START ||
           type == VGFX_EVENT_COMPOSITION_COMMIT || type == VGFX_EVENT_COMPOSITION_CANCEL ||
           type == VGFX_EVENT_TOUCH_UP || type == VGFX_EVENT_TOUCH_CANCEL;
}

/// @brief Test whether an event is cheap to evict during overflow.
/// @details Transient events either supersede earlier state snapshots or are
///          useful but not required for input-state correctness.  They are the
///          first candidates removed when a full queue must make room for a more
///          important state transition.
/// @param type Event type to classify.
/// @return Non-zero when the event is a preferred overflow victim.
static int vgfx_event_is_transient_overflow_candidate(vgfx_event_type_t type) {
    return type == VGFX_EVENT_MOUSE_MOVE || type == VGFX_EVENT_SCROLL ||
           type == VGFX_EVENT_RESIZE || type == VGFX_EVENT_FOCUS_GAINED ||
           type == VGFX_EVENT_TEXT_INPUT || type == VGFX_EVENT_COMPOSITION_UPDATE;
}

/// @brief Test whether an event must survive an explicit queue flush.
/// @details A flush is caller-requested, so most queued work may be discarded,
///          but close and release-like events still repair observable window
///          state and should remain available to the application.
/// @param type Event type to classify.
/// @return Non-zero when the event should remain queued.
static int vgfx_event_survives_flush(vgfx_event_type_t type) {
    return vgfx_event_is_release_state_event(type);
}

/// @brief Advance a ring-buffer index by one slot.
/// @param index Current slot index.
/// @return Next slot index with wraparound applied.
static int vgfx_ring_next_index(int32_t index) {
    return (index + 1) % VGFX_INTERNAL_EVENT_QUEUE_SLOTS;
}

/// @brief Remove one queued event while preserving the order of all others.
/// @details The event queue lock must already be held.  Dropping the oldest
///          event is O(1); middle-slot removals compact later events one slot
///          toward the tail and move `event_head` back to represent the newly
///          freed slot.
/// @param win Window whose event queue is full.
/// @param drop_index Ring-buffer slot to remove.
/// @pre win != NULL.
/// @pre drop_index identifies an occupied slot in win's queue.
static void vgfx_drop_event_at_locked(struct vgfx_window *win, int32_t drop_index) {
    if (drop_index == win->event_tail) {
        win->event_tail = vgfx_ring_next_index(win->event_tail);
        return;
    }

    int32_t cursor = drop_index;
    int32_t next = vgfx_ring_next_index(cursor);
    while (next != win->event_head) {
        win->event_queue[cursor] = win->event_queue[next];
        cursor = next;
        next = vgfx_ring_next_index(next);
    }
    win->event_head = cursor;
}

/// @brief Choose which queued event to evict for a new event.
/// @details Preserves close and release-like events whenever possible.  New close
///          events can evict any older non-close event unless a close is already
///          queued, because a duplicate close adds no observable state.
/// @param win Window whose queue is full.
/// @param new_event Event the caller wants to enqueue.
/// @return Ring-buffer slot to evict, or -1 when the new event should be dropped.
/// @pre win != NULL.
/// @pre new_event != NULL.
/// @pre The event queue lock is held and the queue is full.
static int32_t vgfx_find_overflow_victim_locked(struct vgfx_window *win,
                                                const vgfx_event_t *new_event) {
    if (new_event->type == VGFX_EVENT_CLOSE) {
        for (int32_t index = win->event_tail; index != win->event_head;
             index = vgfx_ring_next_index(index)) {
            if (win->event_queue[index].type == VGFX_EVENT_CLOSE)
                return -1;
        }

        for (int32_t index = win->event_tail; index != win->event_head;
             index = vgfx_ring_next_index(index)) {
            if (win->event_queue[index].type != VGFX_EVENT_CLOSE)
                return index;
        }
        return -1;
    }

    int32_t oldest = win->event_tail;
    if (!vgfx_event_is_release_state_event(win->event_queue[oldest].type))
        return oldest;

    for (int32_t index = vgfx_ring_next_index(oldest); index != win->event_head;
         index = vgfx_ring_next_index(index)) {
        if (vgfx_event_is_transient_overflow_candidate(win->event_queue[index].type))
            return index;
    }

    if (vgfx_event_is_release_state_event(new_event->type)) {
        for (int32_t index = vgfx_ring_next_index(oldest); index != win->event_head;
             index = vgfx_ring_next_index(index)) {
            if (!vgfx_event_is_release_state_event(win->event_queue[index].type))
                return index;
        }
    }

    return -1;
}

/// @brief Enqueue an event into the window's ring buffer.
/// @details Attempts to add the event to the queue.  If the queue is full,
///          evicts one queued event only when doing so preserves important
///          state transitions.  CLOSE, KEY_UP, MOUSE_UP, and FOCUS_LOST events
///          are kept whenever possible so close requests and release state are
///          not lost under bursty input.  Transient events such as mouse move,
///          scroll, resize, and text input are preferred overflow victims.
///
///          This ensures CLOSE events are never lost once enqueued.
///
/// @param win   Pointer to the window structure
/// @param event Pointer to the event to enqueue (copied into queue)
/// @return 1 if event was enqueued, 0 if dropped (queue full)
///
/// @pre  win != NULL
/// @pre  event != NULL
/// @post Event is in queue OR event_overflow incremented (non-CLOSE dropped)
int vgfx_internal_enqueue_event(struct vgfx_window *win, const vgfx_event_t *event) {
    if (!win || !event)
        return 0;

    vgfx_internal_event_lock(win);
    if (win->destroying) {
        vgfx_internal_event_unlock(win);
        return 0;
    }

    /* Native IMEs can publish many preedit selection updates in one platform
       pump. Only the newest consecutive update is observable, while start,
       commit, and cancel boundaries remain distinct queue entries. */
    if (event->type == VGFX_EVENT_COMPOSITION_UPDATE && win->event_head != win->event_tail) {
        int32_t previous =
            (win->event_head == 0) ? (VGFX_INTERNAL_EVENT_QUEUE_SLOTS - 1) : (win->event_head - 1);
        if (win->event_queue[previous].type == VGFX_EVENT_COMPOSITION_UPDATE) {
            win->event_queue[previous] = *event;
            vgfx_internal_event_unlock(win);
            return 1;
        }
    }
    int32_t next_head = vgfx_ring_next_index(win->event_head);

    /* Queue full? */
    if (next_head == win->event_tail) {
        int32_t victim = vgfx_find_overflow_victim_locked(win, event);
        if (victim < 0) {
            if (win->event_overflow < INT32_MAX)
                win->event_overflow++;
            vgfx_internal_event_unlock(win);
            return 0;
        }
        vgfx_drop_event_at_locked(win, victim);
        next_head = vgfx_ring_next_index(win->event_head);
        if (win->event_overflow < INT32_MAX)
            win->event_overflow++;
    }

    /* Enqueue event (queue now has space) */
    win->event_queue[win->event_head] = *event;
    win->event_head = next_head;
    vgfx_internal_event_unlock(win);
    return 1;
}

/// @copydoc vgfx_internal_enqueue_coalesced_event
int vgfx_internal_enqueue_coalesced_event(struct vgfx_window *win, const vgfx_event_t *event) {
    if (!win || !event)
        return 0;
    if (event->type != VGFX_EVENT_MOUSE_MOVE)
        return vgfx_internal_enqueue_event(win, event);

    vgfx_internal_event_lock(win);
    if (win->destroying) {
        vgfx_internal_event_unlock(win);
        return 0;
    }

    if (win->event_head != win->event_tail) {
        int32_t previous =
            (win->event_head == 0) ? (VGFX_INTERNAL_EVENT_QUEUE_SLOTS - 1) : (win->event_head - 1);
        if (win->event_queue[previous].type == VGFX_EVENT_MOUSE_MOVE) {
            win->event_queue[previous] = *event;
            vgfx_internal_event_unlock(win);
            return 1;
        }
    }

    int32_t next_head = vgfx_ring_next_index(win->event_head);
    if (next_head == win->event_tail) {
        int32_t victim = vgfx_find_overflow_victim_locked(win, event);
        if (victim < 0) {
            if (win->event_overflow < INT32_MAX)
                win->event_overflow++;
            vgfx_internal_event_unlock(win);
            return 0;
        }
        vgfx_drop_event_at_locked(win, victim);
        next_head = vgfx_ring_next_index(win->event_head);
        if (win->event_overflow < INT32_MAX)
            win->event_overflow++;
    }

    win->event_queue[win->event_head] = *event;
    win->event_head = next_head;
    vgfx_internal_event_unlock(win);
    return 1;
}

/// @copydoc vgfx_internal_note_event_overflow
void vgfx_internal_note_event_overflow(struct vgfx_window *win) {
    if (!win)
        return;

    vgfx_internal_event_lock(win);
    if (win->destroying) {
        vgfx_internal_event_unlock(win);
        return;
    }
    if (win->event_overflow < INT32_MAX)
        win->event_overflow++;
    vgfx_internal_event_unlock(win);
}

/// @brief Dequeue the next event from the window's ring buffer.
/// @details Removes and returns the oldest event from the queue.  If the queue
///          is empty, returns 0 without modifying out_event.
///
/// @param win       Pointer to the window structure
/// @param out_event Pointer to event storage (filled on success)
/// @return 1 if an event was dequeued, 0 if queue is empty
///
/// @pre  win != NULL
/// @pre  out_event != NULL
/// @post If 1 returned: out_event contains oldest event, event_tail advanced
/// @post If 0 returned: out_event unchanged, queue is empty
int vgfx_internal_dequeue_event(struct vgfx_window *win, vgfx_event_t *out_event) {
    if (!win || !out_event)
        return 0;

    vgfx_internal_event_lock(win);
    /* Queue empty? */
    if (win->event_head == win->event_tail) {
        vgfx_internal_event_unlock(win);
        return 0;
    }

    /* Dequeue event */
    *out_event = win->event_queue[win->event_tail];
    win->event_tail = (win->event_tail + 1) % VGFX_INTERNAL_EVENT_QUEUE_SLOTS;
    vgfx_internal_event_unlock(win);
    return 1;
}

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
/// @post If 1 returned: out_event contains oldest event, queue unchanged
/// @post If 0 returned: out_event unchanged, queue is empty
int vgfx_internal_peek_event(struct vgfx_window *win, vgfx_event_t *out_event) {
    if (!win || !out_event)
        return 0;

    vgfx_internal_event_lock(win);
    /* Queue empty? */
    if (win->event_head == win->event_tail) {
        vgfx_internal_event_unlock(win);
        return 0;
    }

    /* Peek event (don't modify tail) */
    *out_event = win->event_queue[win->event_tail];
    vgfx_internal_event_unlock(win);
    return 1;
}

//===----------------------------------------------------------------------===//
// Version Functions
//===----------------------------------------------------------------------===//

/// @brief Get the library version as a packed 32-bit integer.
/// @details Format: (major << 16) | (minor << 8) | patch
///          Example: Version 1.2.3 returns 0x00010203
///
/// @return Packed version number
uint32_t vgfx_version(void) {
    return (VGFX_VERSION_MAJOR << 16) | (VGFX_VERSION_MINOR << 8) | VGFX_VERSION_PATCH;
}

/// @brief Get the library version as a human-readable string.
/// @details Returns a static string in "major.minor.patch" format.
///
/// @return Pointer to static version string (e.g., "1.0.0")
const char *vgfx_version_string(void) {
    return "1.0.0";
}

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

/// @brief Get the last error message (thread-local).
/// @details Returns the error message set by the most recent error in this
///          thread.  The returned pointer is valid until the next error or
///          until vgfx_clear_error() is called.
///
/// @return Error message string, or NULL if no error has occurred
const char *vgfx_get_last_error(void) {
    return g_last_error_str;
}

/// @brief Clear the thread-local error state.
/// @details Resets the code to VGFX_ERR_NONE and the message to NULL.
///
/// @post vgfx_get_last_error() returns NULL
/// @post vgfx_last_error_code() returns VGFX_ERR_NONE
void vgfx_clear_error(void) {
    g_last_error_str = NULL;
    g_last_error_code = VGFX_ERR_NONE;
}

/// @brief Get the last error code (thread-local).
/// @details Returns the error code set by the most recent error in this thread.
///
/// @return Error code (VGFX_ERR_NONE if no error)
vgfx_error_t vgfx_last_error_code(void) {
    return g_last_error_code;
}

/// @brief Set a user-provided logging callback for error messages.
/// @details The callback is invoked whenever an error occurs. Optional stderr
///          mirroring remains independently controlled by ZANNA_GFX_STDERR.
///          Useful for integrating ZannaGFX errors with application logging.
///
/// @param fn Logging callback function (or NULL to disable)
///
/// @post Future errors will invoke fn(message) if fn != NULL
void vgfx_set_log_callback(vgfx_log_fn fn) {
    vgfx_config_lock();
    g_log_callback = fn;
    vgfx_config_unlock();
}

//===----------------------------------------------------------------------===//
// Configuration Functions
//===----------------------------------------------------------------------===//

/// @brief Set the global default FPS for new windows.
/// @details Changes the default frame rate used when vgfx_window_params_t.fps
///          is 0.  Affects future calls to vgfx_create_window() but does not
///          modify existing windows.
///
/// @param fps Target FPS (clamped to [1, 1000]) or negative for unlimited
///
/// @post vgfx_get_default_fps() returns fps (clamped if positive)
void vgfx_set_default_fps(int32_t fps) {
    vgfx_config_lock();
    if (fps > 0) {
        g_default_fps = clamp_int(fps, 1, 1000);
    } else {
        g_default_fps = fps; /* Allow negative for unlimited */
    }
    vgfx_config_unlock();
}

/// @brief Get the current global default FPS.
/// @details Returns the value set by vgfx_set_default_fps() or the initial
///          default (VGFX_DEFAULT_FPS = 60).
///
/// @return Global default FPS (positive, or negative for unlimited)
int32_t vgfx_get_default_fps(void) {
    return vgfx_read_default_fps();
}

/// @brief Set the FPS limit for an existing window.
/// @details Changes the frame rate limit for vgfx_update().  Takes effect on
///          the next call to vgfx_update().
///
/// @param window Window handle
/// @param fps    Target FPS (0 = use default, positive = target, negative = unlimited)
///
/// @pre  window != NULL
/// @post window->fps is updated to fps (or g_default_fps if fps == 0)
void vgfx_set_fps(vgfx_window_t window, int32_t fps) {
    if (!window)
        return;

    if (fps == 0) {
        window->fps = vgfx_read_default_fps();
    } else if (fps > 0) {
        window->fps = clamp_int(fps, 1, 1000);
    } else {
        window->fps = fps; /* Allow negative for unlimited */
    }

    if (window->fps > 0)
        window->next_frame_deadline_ms =
            (double)vgfx_platform_now_ms() + (1000.0 / (double)window->fps);
}

/// @brief Get the current FPS limit for a window.
/// @details Returns the window's frame rate limit (may differ from global default).
///
/// @param window Window handle
/// @return Window's FPS limit, or -1 if window is NULL
int32_t vgfx_get_fps(vgfx_window_t window) {
    if (!window)
        return -1;
    return window->fps;
}

/// @brief Set the window title.
/// @details Changes the window's title bar text at runtime.
///
/// @param window Window handle
/// @param title  New title string (UTF-8), or NULL for default
void vgfx_set_title(vgfx_window_t window, const char *title) {
    if (!window)
        return;

    const char *actual_title = title ? title : VGFX_DEFAULT_TITLE;
    vgfx_platform_set_title(window, actual_title);
}

/// @brief Register a callback invoked immediately on window resize.
/// @details On macOS the Cocoa live-resize modal loop blocks the main thread;
///          calling the callback from windowDidResize: keeps the window painted
///          during the drag.  On other platforms the callback is stored but
///          never invoked from platform code (resize events arrive via poll).
/// @param window Window whose resize callback is replaced.
/// @param callback Callback receiving userdata and new physical dimensions, or NULL to clear it.
/// @param userdata Opaque callback state retained without ownership.
void vgfx_set_resize_callback(vgfx_window_t window,
                              void (*callback)(void *userdata, int32_t w, int32_t h),
                              void *userdata) {
    if (!window)
        return;
    struct vgfx_window *win = (struct vgfx_window *)window;
    win->on_resize = callback;
    win->on_resize_userdata = userdata;
}

/// @brief Set the window to fullscreen or windowed mode.
/// @details Toggles between fullscreen and windowed modes. The framebuffer
///          may be resized when switching modes.
///
/// @param window     Window handle
/// @param fullscreen 1 for fullscreen, 0 for windowed
void vgfx_set_fullscreen(vgfx_window_t window, int fullscreen) {
    if (!window)
        return;

    vgfx_platform_set_fullscreen(window, fullscreen);
}

/// @brief Check if the window is in fullscreen mode.
/// @param window Window handle
/// @return 1 if fullscreen, 0 if windowed, -1 if window is NULL
int vgfx_is_fullscreen(vgfx_window_t window) {
    if (!window)
        return -1;

    return vgfx_platform_is_fullscreen(window);
}

/// @copydoc vgfx_minimize
void vgfx_minimize(vgfx_window_t window) {
    if (window)
        vgfx_platform_minimize(window);
}

/// @copydoc vgfx_maximize
void vgfx_maximize(vgfx_window_t window) {
    if (window)
        vgfx_platform_maximize(window);
}

/// @copydoc vgfx_restore
void vgfx_restore(vgfx_window_t window) {
    if (window)
        vgfx_platform_restore(window);
}

/// @copydoc vgfx_is_minimized
int32_t vgfx_is_minimized(vgfx_window_t window) {
    if (!window)
        return 0;
    return vgfx_platform_is_minimized(window);
}

/// @copydoc vgfx_is_maximized
int32_t vgfx_is_maximized(vgfx_window_t window) {
    if (!window)
        return 0;
    return vgfx_platform_is_maximized(window);
}

/// @copydoc vgfx_get_position
void vgfx_get_position(vgfx_window_t window, int32_t *out_x, int32_t *out_y) {
    if (!window) {
        if (out_x)
            *out_x = 0;
        if (out_y)
            *out_y = 0;
        return;
    }
    vgfx_platform_get_position(window, out_x, out_y);
}

/// @copydoc vgfx_set_position
void vgfx_set_position(vgfx_window_t window, int32_t x, int32_t y) {
    if (window)
        vgfx_platform_set_position(window, x, y);
}

/// @copydoc vgfx_focus
void vgfx_focus(vgfx_window_t window) {
    if (window)
        vgfx_platform_focus(window);
}

/// @copydoc vgfx_request_foreground
void vgfx_request_foreground(vgfx_window_t window) {
    if (window)
        vgfx_platform_request_foreground(window);
}

/// @copydoc vgfx_is_focused
int32_t vgfx_is_focused(vgfx_window_t window) {
    if (!window)
        return 0;
    return vgfx_platform_is_focused(window);
}

/// @copydoc vgfx_set_prevent_close
void vgfx_set_prevent_close(vgfx_window_t window, int32_t prevent) {
    if (window)
        vgfx_platform_set_prevent_close(window, prevent);
}

/// @copydoc vgfx_set_cursor
void vgfx_set_cursor(vgfx_window_t window, int32_t cursor_type) {
    if (window)
        vgfx_platform_set_cursor(window, cursor_type);
}

/// @copydoc vgfx_set_cursor_visible
void vgfx_set_cursor_visible(vgfx_window_t window, int32_t visible) {
    if (window)
        vgfx_platform_set_cursor_visible(window, visible);
}

/// @copydoc vgfx_get_monitor_size
void vgfx_get_monitor_size(vgfx_window_t window, int32_t *out_w, int32_t *out_h) {
    vgfx_platform_get_monitor_size(window, out_w, out_h);
}

/// @copydoc vgfx_set_window_size
void vgfx_set_window_size(vgfx_window_t window, int32_t w, int32_t h) {
    if (window && w > 0 && h > 0) {
        if (w < window->min_width)
            w = window->min_width;
        if (h < window->min_height)
            h = window->min_height;
        vgfx_platform_set_window_size(window, w, h);
    }
}

/// @copydoc vgfx_set_window_min_size
void vgfx_set_window_min_size(vgfx_window_t window, int32_t w, int32_t h) {
    if (!window)
        return;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w > VGFX_MAX_WIDTH)
        w = VGFX_MAX_WIDTH;
    if (h > VGFX_MAX_HEIGHT)
        h = VGFX_MAX_HEIGHT;
    window->min_width = w;
    window->min_height = h;
    vgfx_platform_set_window_min_size(window, w, h);
}

//===----------------------------------------------------------------------===//
// Window Management
//===----------------------------------------------------------------------===//

/// @brief Create a window parameter structure with default values.
/// @details Fills width, height, title, and FPS from the corresponding
///          VGFX_DEFAULT_* macros and disables resizable/fullscreen creation.
///
/// @return Window parameters initialized with defaults
vgfx_window_params_t vgfx_window_params_default(void) {
    vgfx_window_params_t params;
    params.width = VGFX_DEFAULT_WIDTH;
    params.height = VGFX_DEFAULT_HEIGHT;
    params.title = VGFX_DEFAULT_TITLE;
    params.fps = VGFX_DEFAULT_FPS;
    params.resizable = 0;
    params.fullscreen = 0;
    return params;
}

/// @copydoc vgfx_get_display_size
void vgfx_get_display_size(int32_t *out_w, int32_t *out_h) {
    extern int vgfx_platform_get_display_logical_size(int32_t *w, int32_t *h);
    int32_t w = 0;
    int32_t h = 0;
    if (!vgfx_platform_get_display_logical_size(&w, &h) || w <= 0 || h <= 0) {
        w = VGFX_DEFAULT_WIDTH;
        h = VGFX_DEFAULT_HEIGHT;
    }
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
}

/// @brief Create a new window with the specified parameters.
/// @details Allocates a window structure, framebuffer, and platform resources.
///          The window is immediately visible and ready for rendering.  Returns
///          NULL on failure (sets thread-local error state).
///
///          Parameter defaults are applied for invalid/missing values:
///            - width <= 0:  Use VGFX_DEFAULT_WIDTH
///            - height <= 0: Use VGFX_DEFAULT_HEIGHT
///            - title NULL:  Use VGFX_DEFAULT_TITLE
///            - fps == 0:    Use global default FPS
///
/// @param params Window creation parameters (or NULL for all defaults)
/// @return Window handle on success, NULL on failure (check vgfx_get_last_error())
///
/// @post On success: Window is visible, framebuffer cleared to black with alpha=0xFF
/// @post On failure: Error state set, no resources leaked
vgfx_window_t vgfx_create_window(const vgfx_window_params_t *params) {
    /* Apply defaults if params is NULL or fields are invalid */
    vgfx_window_params_t actual_params;

    if (params) {
        actual_params = *params;
    } else {
        actual_params = vgfx_window_params_default();
    }

    /* Apply defaults for invalid fields */
    if (actual_params.width <= 0) {
        actual_params.width = VGFX_DEFAULT_WIDTH;
    }
    if (actual_params.height <= 0) {
        actual_params.height = VGFX_DEFAULT_HEIGHT;
    }
    if (!actual_params.title) {
        actual_params.title = VGFX_DEFAULT_TITLE;
    }

    /* Fullscreen creation: size the window to the desktop up front so it never
     * appears at a small windowed size (the fullscreen state itself is engaged
     * right after platform init below). Clamp to framebuffer limits. */
    if (actual_params.fullscreen) {
        int32_t disp_w = 0;
        int32_t disp_h = 0;
        vgfx_get_display_size(&disp_w, &disp_h);
        actual_params.width = clamp_int(disp_w, 1, VGFX_MAX_WIDTH);
        actual_params.height = clamp_int(disp_h, 1, VGFX_MAX_HEIGHT);
        actual_params.resizable = 1; /* fullscreen transitions require it */
    }

    /* Validate dimensions against safety limits */
    if (actual_params.width > VGFX_MAX_WIDTH || actual_params.height > VGFX_MAX_HEIGHT) {
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM,
                                "Window dimensions exceed maximum (4096x4096)");
        return NULL;
    }

    /* Allocate window structure */
    struct vgfx_window *win = (struct vgfx_window *)calloc(1, sizeof(struct vgfx_window));
    if (!win) {
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate window structure");
        return NULL;
    }

    /* Initialize window properties */
    win->resizable = actual_params.resizable;
    win->min_width = 1;
    win->min_height = 1;

    /* Query HiDPI scale once and store it.  Scale up the requested logical
     * dimensions to physical pixels so the framebuffer is allocated at the
     * native display resolution.  win->width / win->height are PHYSICAL after
     * this point; divide by scale_factor to recover logical (point) dimensions. */
    float dpi_scale = vgfx_platform_get_display_scale();
    win->scale_factor = vgfx_internal_sanitize_scale(dpi_scale);
    win->width = vgfx_internal_scale_up_i32(actual_params.width, win->scale_factor);
    win->height = vgfx_internal_scale_up_i32(actual_params.height, win->scale_factor);
    if (win->width <= 0 || win->height <= 0 || win->width > VGFX_MAX_WIDTH ||
        win->height > VGFX_MAX_HEIGHT) {
        free(win);
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM,
                                "Scaled window dimensions exceed framebuffer limits");
        return NULL;
    }
    win->stride = win->width * 4;
    win->coord_scale = 1.0f; /* No coordinate scaling by default (GUI layer) */

    /* Set FPS (apply default if params.fps == 0, clamp if positive) */
    if (actual_params.fps == 0) {
        win->fps = vgfx_read_default_fps();
    } else if (actual_params.fps > 0) {
        win->fps = clamp_int(actual_params.fps, 1, 1000);
    } else {
        win->fps = actual_params.fps; /* Negative = unlimited */
    }

    /* Allocate framebuffer (aligned for cache performance) */
    size_t fb_size = 0;
    if (!framebuffer_size_bytes(win->width, win->height, &fb_size)) {
        free(win);
        vgfx_internal_set_error(VGFX_ERR_INVALID_PARAM, "Framebuffer size overflow");
        return NULL;
    }
    win->pixels = (uint8_t *)vgfx_platform_aligned_alloc(VGFX_FRAMEBUFFER_ALIGNMENT, fb_size);
    if (!win->pixels) {
        free(win);
        vgfx_internal_set_error(VGFX_ERR_ALLOC, "Failed to allocate framebuffer");
        return NULL;
    }

    clear_framebuffer_rgba(win->pixels, fb_size);
    win->framebuffer_generation = 1;

    /* Initialize event queue (empty ring buffer) */
    vgfx_atomic_flag_init(&win->event_lock);
    win->event_head = 0;
    win->event_tail = 0;
    win->event_overflow = 0;

    /* Initialize close state */
    win->close_requested = 0;

    /* Initialize input state (all keys/buttons released) */
    vgfx_internal_clear_input_state(win);
    win->mouse_x = 0;
    win->mouse_y = 0;

    /* Initialize timing (start frame deadline at current time) */
    win->last_frame_time_ms = 0;
    win->next_frame_deadline_ms = (double)vgfx_platform_now_ms();
    if (win->fps > 0)
        win->next_frame_deadline_ms += 1000.0 / (double)win->fps;

    /* Initialize platform-specific resources (native window, etc.) */
    if (!vgfx_platform_init_window(win, &actual_params)) {
        vgfx_platform_aligned_free(win->pixels);
        free(win);
        /* Error already set by platform backend */
        return NULL;
    }

    /* Backends that only learn the HiDPI factor once a native surface exists
     * (Wayland reports it through the surface's outputs) publish it during the
     * init above, after win->width was already derived from the pre-window
     * scale queried earlier.  Re-derive the framebuffer from the requested
     * logical size so the window is settled before the first frame instead of
     * resizing underneath the caller's initial paint. */
    if (win->scale_factor != dpi_scale) {
        int32_t settled_w = vgfx_internal_scale_up_i32(actual_params.width, win->scale_factor);
        int32_t settled_h = vgfx_internal_scale_up_i32(actual_params.height, win->scale_factor);
        if (settled_w > 0 && settled_h > 0 && settled_w <= VGFX_MAX_WIDTH &&
            settled_h <= VGFX_MAX_HEIGHT && (settled_w != win->width || settled_h != win->height)) {
            /* A failure here leaves the pre-scale framebuffer in place, which
             * still renders correctly; the next resize retries. */
            (void)vgfx_internal_resize_framebuffer(win, settled_w, settled_h);
        }
    }

    /* Engage fullscreen immediately after creation. The window was already
     * sized to the desktop above, so there is no windowed flash — this just
     * flips the platform window state (borderless/native fullscreen). */
    if (actual_params.fullscreen)
        vgfx_set_fullscreen(win, 1);

    return win;
}

/// @brief Destroy a window and free all associated resources.
/// @details Closes the native window, frees the framebuffer, and deallocates
///          the window structure.  Safe to call with NULL (no-op).
///
/// @param window Window handle (may be NULL)
///
/// @post If window != NULL: Native window closed, all resources freed, handle invalid
void vgfx_destroy_window(vgfx_window_t window) {
    if (!window)
        return;

    vgfx_internal_event_lock(window);
    window->destroying = 1;
    vgfx_internal_event_unlock(window);

    /* Destroy platform resources (native window, platform_data) */
    vgfx_platform_destroy_window(window);

    /* Free framebuffer */
    vgfx_internal_event_lock(window);
    uint8_t *pixels = window->pixels;
    window->pixels = NULL;
    window->width = 0;
    window->height = 0;
    window->stride = 0;
    window->framebuffer_generation++;
    vgfx_internal_event_unlock(window);

    if (pixels) {
        vgfx_platform_aligned_free(pixels);
    }
    vgfx_free_retired_framebuffers(window);

    /* Free window structure */
    free(window);
}

/// @brief Process events, present framebuffer, and perform frame limiting.
/// @details Single call that performs a complete frame update:
///            1. Process OS events (keyboard, mouse, window events)
///            2. Present (blit) the framebuffer to the screen
///            3. Sleep if necessary to maintain target FPS
///            4. Update frame timing statistics
///
///          If fps > 0, sleeps until the next frame deadline to maintain the
///          target frame rate.  If fps < 0, returns immediately (unlimited FPS).
///
/// @param window Window handle
/// @return 1 on success, 0 on failure (e.g., event processing error)
///
/// @pre  window != NULL
/// @post OS events processed and queued
/// @post Framebuffer contents visible on screen
/// @post If fps > 0: slept until frame deadline
/// @post window->last_frame_time_ms updated with frame duration
int32_t vgfx_update(vgfx_window_t window) {
    if (!window)
        return 0;

    int64_t frame_start = vgfx_platform_now_ms();

    /* Pump native events first so resize, close, and input state observed by the
     * platform backend is current before presenting the next framebuffer. */
    if (!vgfx_pump_events(window))
        return 0;

    if (!vgfx_platform_present(window)) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Failed to present framebuffer");
        return 0;
    }
    vgfx_embed_present_tap(window);

    /* FPS limiting (only if fps > 0). No idle floor here: vgfx_update() is the
     * game/render loop entry point, and unlimited FPS must stay uncapped. */
    vgfx_frame_pace(window, 0);

    /* Calculate frame time (for diagnostics) */
    int64_t frame_end = vgfx_platform_now_ms();
    window->last_frame_time_ms = frame_end - frame_start;

    return 1;
}

/// @brief Apply frame-rate pacing for a window without presenting a frame.
/// @details Runs the same deadline-based sleep that vgfx_update() performs
///          after presenting: when the window has a positive FPS cap, it sleeps
///          until the next frame deadline, advancing the deadline additively to
///          avoid drift and resyncing if it fell more than one frame behind.
///          When the window has no FPS cap (fps <= 0) it sleeps
///          @p min_idle_sleep_ms milliseconds if that is positive, otherwise it
///          returns immediately. GUI applications call this on frames where
///          nothing needed repainting so an idle window does not busy-loop a
///          CPU core; passing a small positive floor there keeps idle CPU near
///          zero even when the window is running uncapped.
/// @param window Window handle (NULL is a no-op).
/// @param min_idle_sleep_ms Anti-spin floor applied only when fps <= 0.
void vgfx_frame_pace(vgfx_window_t window, int32_t min_idle_sleep_ms) {
    if (!window)
        return;

    if (window->fps > 0) {
        int64_t now = vgfx_platform_now_ms();
        double target_frame_time = 1000.0 / (double)window->fps;

        /* Sleep if we're ahead of schedule */
        if ((double)now < window->next_frame_deadline_ms) {
            double remaining = window->next_frame_deadline_ms - (double)now;
            int32_t sleep_time = (int32_t)remaining;
            if ((double)sleep_time < remaining)
                sleep_time++;
            vgfx_platform_sleep_ms(sleep_time);
            now = vgfx_platform_now_ms();
        }

        /* Update deadline for next frame (additive to avoid drift) */
        window->next_frame_deadline_ms += target_frame_time;

        /* Resync if we fell behind by more than one frame (prevents runaway) */
        if (window->next_frame_deadline_ms < (double)now - target_frame_time) {
            window->next_frame_deadline_ms = (double)now + target_frame_time;
        }
    } else if (min_idle_sleep_ms > 0) {
        vgfx_platform_sleep_ms(min_idle_sleep_ms);
    }
}

/// @brief Get the duration of the last frame in milliseconds.
/// @details Returns the time elapsed during the most recent vgfx_update() call,
///          including event processing, rendering, and sleep time.
///
/// @param window Window handle
/// @return Last frame duration in milliseconds, or -1 if window is NULL
int32_t vgfx_frame_time_ms(vgfx_window_t window) {
    if (!window)
        return -1;
    int64_t frame_time = window->last_frame_time_ms;
    if (frame_time > INT32_MAX)
        return INT32_MAX;
    if (frame_time < INT32_MIN)
        return INT32_MIN;
    return (int32_t)frame_time;
}

/// @copydoc vgfx_pump_events
//===----------------------------------------------------------------------===//
// Embedded-play tap (ADR 0225)
//
// When ZANNA_EMBED_CHANNEL names a live channel, the shared layer mirrors
// every presented framebuffer into the channel and injects channel input
// into the ordinary event queue. The game keeps its real platform window
// and event loop; nothing game-side changes.
//===----------------------------------------------------------------------===//

/// @brief Process-wide embed producer state (lazy first-present attach).
static vgfx_embed_channel_t *g_embed_channel = NULL;
static int g_embed_state = 0; /* 0 = untried, 1 = attached, -1 = unavailable */

/// @brief Attach to the host channel once per process.
static vgfx_embed_channel_t *vgfx_embed_get(void) {
    if (g_embed_state != 0)
        return g_embed_channel;
    const char *name = getenv(VGFX_EMBED_CHANNEL_ENV);
    if (!name || !name[0]) {
        g_embed_state = -1;
        return NULL;
    }
    if (!vgfx_embed_channel_attach(name, &g_embed_channel)) {
        g_embed_state = -1;
        return NULL;
    }
    g_embed_state = 1;
    return g_embed_channel;
}

/// @brief Mirror one presented framebuffer into the embed channel.
static void vgfx_embed_present_tap(vgfx_window_t window) {
    vgfx_embed_channel_t *channel = vgfx_embed_get();
    if (!channel)
        return;
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(window, &fb) || !fb.pixels)
        return;
    (void)vgfx_embed_channel_publish_frame(channel, fb.pixels, fb.width, fb.height);
}

/// @brief Inject queued channel input into the window's event stream.
static void vgfx_embed_input_tap(vgfx_window_t window) {
    vgfx_embed_channel_t *channel = vgfx_embed_get();
    if (!channel)
        return;
    vgfx_embed_event_t in;
    int budget = 64; /* bound one pump's injection work */
    while (budget-- > 0 && vgfx_embed_channel_poll_event(channel, &in)) {
        vgfx_event_t out;
        memset(&out, 0, sizeof(out));
        switch (in.kind) {
        case VGFX_EMBED_EVENT_MOUSE_MOVE:
            out.type = VGFX_EVENT_MOUSE_MOVE;
            out.data.mouse_move.x = in.a;
            out.data.mouse_move.y = in.b;
            break;
        case VGFX_EMBED_EVENT_MOUSE_DOWN:
        case VGFX_EMBED_EVENT_MOUSE_UP:
            out.type = in.kind == VGFX_EMBED_EVENT_MOUSE_DOWN ? VGFX_EVENT_MOUSE_DOWN
                                                              : VGFX_EVENT_MOUSE_UP;
            out.data.mouse_button.x = in.a;
            out.data.mouse_button.y = in.b;
            out.data.mouse_button.button = (vgfx_mouse_button_t)in.c;
            break;
        case VGFX_EMBED_EVENT_WHEEL:
            out.type = VGFX_EVENT_SCROLL;
            out.data.scroll.x = in.a;
            out.data.scroll.y = in.b;
            out.data.scroll.delta_y = (float)in.c / 120.0f;
            break;
        case VGFX_EMBED_EVENT_KEY_DOWN:
        case VGFX_EMBED_EVENT_KEY_UP:
            out.type = in.kind == VGFX_EMBED_EVENT_KEY_DOWN ? VGFX_EVENT_KEY_DOWN
                                                            : VGFX_EVENT_KEY_UP;
            out.data.key.key = (vgfx_key_t)in.a;
            out.data.key.modifiers = in.b;
            break;
        case VGFX_EMBED_EVENT_TEXT:
            out.type = VGFX_EVENT_TEXT_INPUT;
            out.data.text.codepoint = (uint32_t)in.a;
            break;
        case VGFX_EMBED_EVENT_RESIZE:
            vgfx_set_window_size(window, in.a, in.b);
            continue;
        case VGFX_EMBED_EVENT_CLOSE:
            out.type = VGFX_EVENT_CLOSE;
            break;
        default:
            continue;
        }
        (void)vgfx_internal_enqueue_event(window, &out);
    }
}

int32_t vgfx_pump_events(vgfx_window_t window) {
    if (!window)
        return 0;
    if (!vgfx_platform_process_events(window)) {
        vgfx_internal_set_error(VGFX_ERR_PLATFORM, "Event processing error");
        return 0;
    }
    vgfx_embed_input_tap(window);
    return 1;
}

/// @copydoc vgfx_wait_events
int32_t vgfx_wait_events(vgfx_window_t window, int32_t timeout_ms) {
    if (!window)
        return 0;
    if (timeout_ms < 0)
        timeout_ms = 0;
    if (timeout_ms > 1000)
        timeout_ms = 1000; /* hard cap: a bug can never hang the UI for long */
    if (timeout_ms == 0)
        return 0;
    return vgfx_platform_wait_events(window, timeout_ms);
}

/// @copydoc vgfx_set_text_input_enabled
int vgfx_set_text_input_enabled(vgfx_window_t window, int32_t enabled) {
    if (!window || (enabled != 0 && enabled != 1))
        return 0;
    return vgfx_platform_set_text_input_enabled(window, enabled);
}

/// @copydoc vgfx_set_text_input_state
int vgfx_set_text_input_state(vgfx_window_t window, const vgfx_text_input_state_t *state) {
    if (!window || !state || state->cursor_byte < 0 || state->anchor_byte < 0 ||
        state->cursor_width < 0 || state->cursor_height < 0 ||
        state->purpose < VGFX_TEXT_INPUT_NORMAL || state->purpose > VGFX_TEXT_INPUT_URL)
        return 0;
    size_t length = state->surrounding_text ? strlen(state->surrounding_text) : 0;
    if ((size_t)state->cursor_byte > length || (size_t)state->anchor_byte > length)
        return 0;
    return vgfx_platform_set_text_input_state(window, state);
}

/// @brief Get the window's public coordinate-space dimensions.
/// @details Returns logical extents when coordinate scaling is enabled and
///          physical framebuffer extents otherwise.
///
/// @param window Window handle
/// @param width  Pointer to store width (may be NULL)
/// @param height Pointer to store height (may be NULL)
/// @return 1 on success, 0 if window is NULL
///
/// @post If width != NULL: *width contains window width
/// @post If height != NULL: *height contains window height
int32_t vgfx_get_size(vgfx_window_t window, int32_t *width, int32_t *height) {
    if (!window)
        return 0;

    if (width)
        *width = vgfx_internal_public_extent_i32(window->width, window);
    if (height)
        *height = vgfx_internal_public_extent_i32(window->height, window);
    return 1;
}

/// @brief Query the HiDPI backing scale factor for a window.
/// @details Returns the current ratio stored in win->scale_factor. On a 2×
///          macOS Retina display this is 2.0; on 96 DPI it is 1.0. Backends
///          may refresh this value when the window moves between displays.
/// @param window Window to inspect; NULL returns unity.
/// @return Sanitized physical-pixels-per-logical-point ratio.
float vgfx_window_get_scale(vgfx_window_t window) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return 1.0f;
    return vgfx_internal_sanitize_scale(win->scale_factor);
}

/// @copydoc vgfx_set_coord_scale
void vgfx_set_coord_scale(vgfx_window_t window, float scale) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    if (!win)
        return;
    win->coord_scale = vgfx_internal_sanitize_scale(scale);
    // Recording the selection is what lets a later backing-scale change follow
    // this window; the untouched default must not. See
    // vgfx_internal_refresh_scale_factor().
    win->coord_scale_explicit = 1;
}

/// @brief Get the physical pixel width of the window framebuffer.
/// @param window Window to inspect.
/// @return Physical pixel width, or zero for NULL.
int32_t vgfx_window_get_width(vgfx_window_t window) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    return win ? win->width : 0;
}

/// @brief Get the physical pixel height of the window framebuffer.
/// @param window Window to inspect.
/// @return Physical pixel height, or zero for NULL.
int32_t vgfx_window_get_height(vgfx_window_t window) {
    struct vgfx_window *win = (struct vgfx_window *)window;
    return win ? win->height : 0;
}

//===----------------------------------------------------------------------===//
// Event Handling
//===----------------------------------------------------------------------===//

/// @brief Append a validated caller-authored event to the public event queue.
/// @details The queue owns a value copy, so stack-authored events and inline text payloads
///          remain valid after this call returns. Routing through the internal synchronized
///          enqueue operation preserves native ordering, overflow accounting, and release-event
///          protection. Caller injection currently accepts the contiguous public range from
///          KEY_DOWN through FILE_DROP; NONE, touch events, and out-of-range discriminators are
///          rejected before the queue is touched.
/// @param window Window whose synchronized event queue receives the value.
/// @param event Caller-owned event to copy.
/// @return 1 on successful enqueue, otherwise 0.
int32_t vgfx_post_event(vgfx_window_t window, const vgfx_event_t *event) {
    if (!window || !event || event->type <= VGFX_EVENT_NONE || event->type > VGFX_EVENT_FILE_DROP)
        return 0;
    return vgfx_internal_enqueue_event((struct vgfx_window *)window, event);
}

/// @brief Poll the next event from the window's event queue.
/// @details Dequeues and returns the oldest event.  If the queue is empty,
///          returns 0 without modifying out_event.  Events are generated by
///          vgfx_update() calling vgfx_platform_process_events().
///
/// @param window    Window handle
/// @param out_event Pointer to event storage (filled on success)
/// @return 1 if an event was retrieved, 0 if queue is empty or window is NULL
///
/// @pre  window != NULL
/// @pre  out_event != NULL
/// @post If 1 returned: out_event contains oldest event, event removed from queue
/// @post If 0 returned: out_event unchanged, queue is empty
int32_t vgfx_poll_event(vgfx_window_t window, vgfx_event_t *out_event) {
    if (!window || !out_event)
        return 0;
    return vgfx_internal_dequeue_event(window, out_event);
}

/// @brief Peek at the next event without removing it from the queue.
/// @details Returns the oldest event without dequeuing it.  Useful for checking
///          if a specific event type is pending.
///
/// @param window    Window handle
/// @param out_event Pointer to event storage (filled on success)
/// @return 1 if an event is available, 0 if queue is empty or window is NULL
///
/// @pre  window != NULL
/// @pre  out_event != NULL
/// @post If 1 returned: out_event contains oldest event, queue unchanged
/// @post If 0 returned: out_event unchanged, queue is empty
int32_t vgfx_peek_event(vgfx_window_t window, vgfx_event_t *out_event) {
    if (!window || !out_event)
        return 0;
    return vgfx_internal_peek_event(window, out_event);
}

/// @brief Discard non-critical events from the window's event queue.
/// @details Dequeues and discards all pending events.  Useful for ignoring
///          accumulated events (e.g., after a pause or dialog).  Close,
///          key/button release, and focus-lost events are preserved because
///          they repair observable application state.
///
/// @param window Window handle
/// @return Number of non-critical events discarded, or 0 if window is NULL
///
/// @post Only critical state-repair events remain queued.
int32_t vgfx_flush_events(vgfx_window_t window) {
    if (!window)
        return 0;

    vgfx_internal_event_lock(window);
    int32_t dropped = 0;
    int32_t write = window->event_tail;
    for (int32_t read = window->event_tail; read != window->event_head;
         read = vgfx_ring_next_index(read)) {
        vgfx_event_t event = window->event_queue[read];
        if (vgfx_event_survives_flush(event.type)) {
            window->event_queue[write] = event;
            write = vgfx_ring_next_index(write);
        } else {
            dropped++;
        }
    }
    window->event_head = write;
    vgfx_internal_event_unlock(window);
    return dropped;
}

/// @copydoc vgfx_clear_events
void vgfx_clear_events(vgfx_window_t window) {
    (void)vgfx_flush_events(window);
}

/// @brief Get and reset the event overflow counter.
/// @details Returns the number of events dropped due to queue overflow since
///          the last call to this function.  The counter is reset to zero
///          after reading.  Non-zero values indicate the queue was too small
///          or vgfx_poll_event() was not called frequently enough.
///
/// @param window Window handle
/// @return Number of events dropped since last query, or 0 if window is NULL
///
/// @post window->event_overflow == 0
int32_t vgfx_event_overflow_count(vgfx_window_t window) {
    if (!window)
        return 0;

    vgfx_internal_event_lock(window);
    int32_t count = window->event_overflow;
    window->event_overflow = 0; /* Reset after reading */
    vgfx_internal_event_unlock(window);
    return count;
}

/// @copydoc vgfx_close_requested
int32_t vgfx_close_requested(vgfx_window_t window) {
    if (!window)
        return 0;

    vgfx_internal_event_lock(window);
    int32_t requested = window->close_requested;
    vgfx_internal_event_unlock(window);
    return requested;
}

//===----------------------------------------------------------------------===//
// Drawing Operations
//===----------------------------------------------------------------------===//

/// @brief Set a single pixel to the specified color.
/// @details Directly writes to the framebuffer at (x, y).  Alpha is always
///          set to 0xFF (fully opaque).  Silent no-op if coordinates are out
///          of bounds or window is NULL.
///
/// @param window Window handle
/// @param x      X coordinate in pixels
/// @param y      Y coordinate in pixels
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @post If (x, y) in bounds: pixel at (x, y) is set to color with alpha=0xFF
void vgfx_pset(vgfx_window_t window, int32_t x, int32_t y, vgfx_color_t color) {
    if (!window)
        return;

    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        /* HiDPI: fill a cs×cs block at the scaled position */
        int32_t px = vgfx_internal_scale_up_i32(x, cs);
        int32_t py = vgfx_internal_scale_up_i32(y, cs);
        int32_t sz = vgfx_internal_scale_up_i32(1, cs);
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = (color >> 0) & 0xFF;
        for (int32_t dy = 0; dy < sz; dy++) {
            for (int32_t dx = 0; dx < sz; dx++) {
                int32_t bx = px + dx, by = py + dy;
                if (vgfx_internal_in_effective_clip(window, bx, by)) {
                    size_t off = (size_t)by * (size_t)window->stride + (size_t)bx * 4u;
                    window->pixels[off + 0] = r;
                    window->pixels[off + 1] = g;
                    window->pixels[off + 2] = b;
                    window->pixels[off + 3] = 0xFF;
                }
            }
        }
        return;
    }

    /* Standard 1:1 path */
    if (!vgfx_internal_in_effective_clip(window, x, y))
        return;

    size_t offset = (size_t)y * (size_t)window->stride + (size_t)x * 4u;
    window->pixels[offset + 0] = (color >> 16) & 0xFF; /* R */
    window->pixels[offset + 1] = (color >> 8) & 0xFF;  /* G */
    window->pixels[offset + 2] = (color >> 0) & 0xFF;  /* B */
    window->pixels[offset + 3] = 0xFF;                 /* A (fully opaque) */
}

/// @brief Alpha-blend one scaled logical pixel block into the framebuffer.
/// @param window Destination window.
/// @param px Physical left coordinate.
/// @param py Physical top coordinate.
/// @param sz Physical width and height of the square block.
/// @param color Source color encoded as 0xAARRGGBB.
static void vgfx_pset_alpha_block(
    vgfx_window_t window, int32_t px, int32_t py, int32_t sz, uint32_t color) {
    uint8_t src_a = (uint8_t)((color >> 24) & 0xFF);
    if (src_a == 0)
        return;
    uint8_t src_r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t src_g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t src_b = (uint8_t)(color & 0xFF);

    for (int32_t dy = 0; dy < sz; dy++) {
        for (int32_t dx = 0; dx < sz; dx++) {
            int32_t bx = px + dx, by = py + dy;
            if (!vgfx_internal_in_effective_clip(window, bx, by))
                continue;
            size_t off = (size_t)by * (size_t)window->stride + (size_t)bx * 4u;
            if (src_a == 0xFF) {
                window->pixels[off + 0] = src_r;
                window->pixels[off + 1] = src_g;
                window->pixels[off + 2] = src_b;
                window->pixels[off + 3] = 0xFF;
            } else {
                uint32_t inv_a = 255u - src_a;
                window->pixels[off + 0] =
                    (uint8_t)((src_r * src_a + window->pixels[off + 0] * inv_a) / 255u);
                window->pixels[off + 1] =
                    (uint8_t)((src_g * src_a + window->pixels[off + 1] * inv_a) / 255u);
                window->pixels[off + 2] =
                    (uint8_t)((src_b * src_a + window->pixels[off + 2] * inv_a) / 255u);
                window->pixels[off + 3] = 0xFF;
            }
        }
    }
}

/// @copydoc vgfx_pset_alpha
void vgfx_pset_alpha(vgfx_window_t window, int32_t x, int32_t y, uint32_t color) {
    if (!window)
        return;

    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        vgfx_pset_alpha_block(window,
                              vgfx_internal_scale_up_i32(x, cs),
                              vgfx_internal_scale_up_i32(y, cs),
                              vgfx_internal_scale_up_i32(1, cs),
                              color);
        return;
    }

    if (!vgfx_internal_in_effective_clip(window, x, y))
        return;

    uint8_t src_a = (uint8_t)((color >> 24) & 0xFF);

    /* Fast path: fully opaque source avoids multiply/divide */
    if (src_a == 0xFF) {
        size_t offset = (size_t)y * (size_t)window->stride + (size_t)x * 4u;
        window->pixels[offset + 0] = (uint8_t)((color >> 16) & 0xFF); /* R */
        window->pixels[offset + 1] = (uint8_t)((color >> 8) & 0xFF);  /* G */
        window->pixels[offset + 2] = (uint8_t)(color & 0xFF);         /* B */
        window->pixels[offset + 3] = 0xFF;
        return;
    }

    /* Fully transparent: no-op */
    if (src_a == 0)
        return;

    /* Source-over composite */
    uint8_t src_r = (uint8_t)((color >> 16) & 0xFF);
    uint8_t src_g = (uint8_t)((color >> 8) & 0xFF);
    uint8_t src_b = (uint8_t)(color & 0xFF);

    size_t offset = (size_t)y * (size_t)window->stride + (size_t)x * 4u;
    uint8_t dst_r = window->pixels[offset + 0];
    uint8_t dst_g = window->pixels[offset + 1];
    uint8_t dst_b = window->pixels[offset + 2];

    uint32_t inv_a = 255u - src_a;
    window->pixels[offset + 0] = (uint8_t)((src_r * src_a + dst_r * inv_a) / 255u);
    window->pixels[offset + 1] = (uint8_t)((src_g * src_a + dst_g * inv_a) / 255u);
    window->pixels[offset + 2] = (uint8_t)((src_b * src_a + dst_b * inv_a) / 255u);
    window->pixels[offset + 3] = 0xFF;
}

/// @brief Get the color of a single pixel.
/// @details Reads the RGB color from the framebuffer at (x, y).  Alpha channel
///          is ignored (always fully opaque in ZannaGFX v1).
///
/// @param window    Window handle
/// @param x         X coordinate in pixels
/// @param y         Y coordinate in pixels
/// @param out_color Pointer to store color (format: 0x00RRGGBB)
/// @return 1 on success, 0 if out of bounds, window is NULL, or out_color is NULL
///
/// @post If 1 returned: *out_color contains RGB color at (x, y)
int32_t vgfx_point(vgfx_window_t window, int32_t x, int32_t y, vgfx_color_t *out_color) {
    if (!window || !out_color)
        return 0;

    float cs = vgfx_internal_coord_scale(window);
    int32_t px = (cs > 1.0f) ? vgfx_internal_scale_up_i32(x, cs) : x;
    int32_t py = (cs > 1.0f) ? vgfx_internal_scale_up_i32(y, cs) : y;

    if (!vgfx_internal_in_bounds(window, px, py))
        return 0;

    size_t offset = (size_t)py * (size_t)window->stride + (size_t)px * 4u;
    uint8_t r = window->pixels[offset + 0];
    uint8_t g = window->pixels[offset + 1];
    uint8_t b = window->pixels[offset + 2];

    *out_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return 1;
}

/// @brief Clear the entire framebuffer to a solid color.
/// @details Sets the active clip, or the entire framebuffer when unclipped, to
///          the specified color with alpha=0xFF.
///
/// @param window Window handle
/// @param color  RGB color (format: 0x00RRGGBB)
///
/// @post Active clip pixels, or all pixels when clipping is disabled, are set to color with
/// alpha=0xFF
void vgfx_cls(vgfx_window_t window, vgfx_color_t color) {
    if (!window)
        return;

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 0) & 0xFF;

    if (!window->clip_enabled) {
        size_t pixel_count = (size_t)window->width * (size_t)window->height;
        fill_rgba_pixels(window->pixels, pixel_count, r, g, b, 0xFF);
        return;
    }

    if (window->clip_w <= 0 || window->clip_h <= 0)
        return;

    int64_t left = window->clip_x;
    int64_t top = window->clip_y;
    int64_t right = (int64_t)window->clip_x + (int64_t)window->clip_w;
    int64_t bottom = (int64_t)window->clip_y + (int64_t)window->clip_h;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > window->width)
        right = window->width;
    if (bottom > window->height)
        bottom = window->height;
    if (left >= right || top >= bottom)
        return;

    for (int64_t y = top; y < bottom; y++) {
        uint8_t *row = window->pixels + (size_t)y * (size_t)window->stride + (size_t)left * 4u;
        fill_rgba_pixels(row, (size_t)(right - left), r, g, b, 0xFF);
    }
}

//===----------------------------------------------------------------------===//
// Drawing Primitives (Forwarding to vgfx_draw.c)
//===----------------------------------------------------------------------===//
// These functions are implemented in vgfx_draw.c using Bresenham and midpoint
// algorithms.  The public API functions forward to the internal implementations.
//===----------------------------------------------------------------------===//

/* Forward declarations for drawing primitives (implemented in vgfx_draw.c) */
/// @copydoc vgfx_draw_line
void vgfx_draw_line(
    vgfx_window_t window, int32_t x1, int32_t y1, int32_t x2, int32_t y2, vgfx_color_t color);
/// @copydoc vgfx_draw_rect
void vgfx_draw_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color);
/// @copydoc vgfx_draw_fill_rect
void vgfx_draw_fill_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color);
/// @copydoc vgfx_draw_circle
void vgfx_draw_circle(
    vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color);
/// @copydoc vgfx_draw_fill_circle
void vgfx_draw_fill_circle(
    vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color);

/// @brief Draw a line from (x1, y1) to (x2, y2).
/// @details Uses Bresenham's line algorithm (implemented in vgfx_draw.c).
///
/// @param window Window handle
/// @param x1     Start X coordinate
/// @param y1     Start Y coordinate
/// @param x2     End X coordinate
/// @param y2     End Y coordinate
/// @param color  RGB color (format: 0x00RRGGBB)
void vgfx_line(
    vgfx_window_t window, int32_t x1, int32_t y1, int32_t x2, int32_t y2, vgfx_color_t color) {
    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        vgfx_draw_line(window,
                       vgfx_internal_scale_up_i32(x1, cs),
                       vgfx_internal_scale_up_i32(y1, cs),
                       vgfx_internal_scale_up_i32(x2, cs),
                       vgfx_internal_scale_up_i32(y2, cs),
                       color);
        return;
    }
    vgfx_draw_line(window, x1, y1, x2, y2, color);
}

/// @brief Draw an unfilled rectangle.
/// @details Draws the outline of a rectangle with top-left corner at (x, y)
///          and dimensions w × h.  Implemented in vgfx_draw.c.
///
/// @param window Window handle
/// @param x      Top-left X coordinate
/// @param y      Top-left Y coordinate
/// @param w      Width in pixels
/// @param h      Height in pixels
/// @param color  RGB color (format: 0x00RRGGBB)
void vgfx_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color) {
    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        vgfx_draw_rect(window,
                       vgfx_internal_scale_up_i32(x, cs),
                       vgfx_internal_scale_up_i32(y, cs),
                       vgfx_internal_scale_up_i32(w, cs),
                       vgfx_internal_scale_up_i32(h, cs),
                       color);
        return;
    }
    vgfx_draw_rect(window, x, y, w, h, color);
}

/// @brief Draw a filled rectangle.
/// @details Fills a rectangle with top-left corner at (x, y) and dimensions
///          w × h.  Implemented in vgfx_draw.c.
///
/// @param window Window handle
/// @param x      Top-left X coordinate
/// @param y      Top-left Y coordinate
/// @param w      Width in pixels
/// @param h      Height in pixels
/// @param color  RGB color (format: 0x00RRGGBB)
void vgfx_fill_rect(
    vgfx_window_t window, int32_t x, int32_t y, int32_t w, int32_t h, vgfx_color_t color) {
    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        vgfx_draw_fill_rect(window,
                            vgfx_internal_scale_up_i32(x, cs),
                            vgfx_internal_scale_up_i32(y, cs),
                            vgfx_internal_scale_up_i32(w, cs),
                            vgfx_internal_scale_up_i32(h, cs),
                            color);
        return;
    }
    vgfx_draw_fill_rect(window, x, y, w, h, color);
}

/// @brief Draw an unfilled circle.
/// @details Draws the outline of a circle centered at (cx, cy) with the
///          specified radius.  Uses midpoint circle algorithm (vgfx_draw.c).
///
/// @param window Window handle
/// @param cx     Center X coordinate
/// @param cy     Center Y coordinate
/// @param radius Radius in pixels
/// @param color  RGB color (format: 0x00RRGGBB)
void vgfx_circle(vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color) {
    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        vgfx_draw_circle(window,
                         vgfx_internal_scale_up_i32(cx, cs),
                         vgfx_internal_scale_up_i32(cy, cs),
                         vgfx_internal_scale_up_i32(radius, cs),
                         color);
        return;
    }
    vgfx_draw_circle(window, cx, cy, radius, color);
}

/// @brief Draw a filled circle.
/// @details Fills a circle centered at (cx, cy) with the specified radius.
///          Uses scanline filling algorithm (vgfx_draw.c).
///
/// @param window Window handle
/// @param cx     Center X coordinate
/// @param cy     Center Y coordinate
/// @param radius Radius in pixels
/// @param color  RGB color (format: 0x00RRGGBB)
void vgfx_fill_circle(
    vgfx_window_t window, int32_t cx, int32_t cy, int32_t radius, vgfx_color_t color) {
    float cs = vgfx_internal_coord_scale(window);
    if (cs > 1.0f) {
        vgfx_draw_fill_circle(window,
                              vgfx_internal_scale_up_i32(cx, cs),
                              vgfx_internal_scale_up_i32(cy, cs),
                              vgfx_internal_scale_up_i32(radius, cs),
                              color);
        return;
    }
    vgfx_draw_fill_circle(window, cx, cy, radius, color);
}

//===----------------------------------------------------------------------===//
// Color Utilities
//===----------------------------------------------------------------------===//

/// @brief Extract RGB components from a packed color value.
/// @details Splits a vgfx_color_t (0x00RRGGBB) into separate R, G, B bytes.
///
/// @param color Packed RGB color (format: 0x00RRGGBB)
/// @param r     Pointer to store red component (may be NULL)
/// @param g     Pointer to store green component (may be NULL)
/// @param b     Pointer to store blue component (may be NULL)
///
/// @post If r != NULL: *r contains red component [0, 255]
/// @post If g != NULL: *g contains green component [0, 255]
/// @post If b != NULL: *b contains blue component [0, 255]
void vgfx_color_to_rgb(vgfx_color_t color, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (r)
        *r = (color >> 16) & 0xFF;
    if (g)
        *g = (color >> 8) & 0xFF;
    if (b)
        *b = (color >> 0) & 0xFF;
}

//===----------------------------------------------------------------------===//
// Input Polling
//===----------------------------------------------------------------------===//

/// @brief Check if a key is currently pressed.
/// @details Returns the current state of the specified key.  Updated by
///          vgfx_update() -> vgfx_platform_process_events().
///
/// @param window Window handle
/// @param key    Key code (must be < 512 and != VGFX_KEY_UNKNOWN)
/// @return 1 if key is pressed, 0 if released, window is NULL, or key is invalid
///
/// @pre  key < 512
int32_t vgfx_key_down(vgfx_window_t window, vgfx_key_t key) {
    int key_index = (int)key;
    if (!window || key_index <= (int)VGFX_KEY_UNKNOWN || key_index >= 512)
        return 0;
    vgfx_internal_event_lock(window);
    int32_t down = window->key_state[key_index] != 0;
    vgfx_internal_event_unlock(window);
    return down;
}

/// @brief Get the current mouse cursor position.
/// @details Returns the mouse position in window-relative coordinates.  The
///          position may be outside [0, width) × [0, height) if the cursor
///          is outside the window.
///
/// @param window Window handle
/// @param x      Pointer to store X coordinate (may be NULL)
/// @param y      Pointer to store Y coordinate (may be NULL)
/// @return 1 if cursor is inside the window bounds, 0 otherwise or if window is NULL
///
/// @post If x != NULL: *x contains cursor X coordinate
/// @post If y != NULL: *y contains cursor Y coordinate
int32_t vgfx_mouse_pos(vgfx_window_t window, int32_t *x, int32_t *y) {
    if (!window)
        return 0;

    vgfx_internal_event_lock(window);
    float cs = vgfx_internal_coord_scale(window);
    int32_t mx = window->mouse_x;
    int32_t my = window->mouse_y;
    int32_t width = window->width;
    int32_t height = window->height;
    vgfx_internal_event_unlock(window);

    /* Return logical coordinates when coord_scale is active */
    if (cs > 1.0f) {
        mx = vgfx_internal_scale_down_i32(mx, cs);
        my = vgfx_internal_scale_down_i32(my, cs);
    }

    if (x)
        *x = mx;
    if (y)
        *y = my;

    /* Logical bounds check */
    int32_t lw = (cs > 1.0f) ? vgfx_internal_scale_down_i32(width, cs) : width;
    int32_t lh = (cs > 1.0f) ? vgfx_internal_scale_down_i32(height, cs) : height;
    return (mx >= 0 && mx < lw && my >= 0 && my < lh);
}

/// @brief Check if a mouse button is currently pressed.
/// @details Returns the current state of the specified mouse button.  Updated
///          by vgfx_update() -> vgfx_platform_process_events().
///
/// @param window Window handle
/// @param button Mouse button code (must be < 8)
/// @return 1 if button is pressed, 0 if released, window is NULL, or button is invalid
///
/// @pre  button < 8
int32_t vgfx_mouse_button(vgfx_window_t window, vgfx_mouse_button_t button) {
    int button_index = (int)button;
    if (!window || button_index < 0 || button_index >= 8)
        return 0;
    vgfx_internal_event_lock(window);
    int32_t down = window->mouse_button_state[button_index] != 0;
    vgfx_internal_event_unlock(window);
    return down;
}

/// @copydoc vgfx_warp_cursor
void vgfx_warp_cursor(vgfx_window_t window, int32_t x, int32_t y) {
    if (!window)
        return;
    float cs = vgfx_internal_coord_scale(window);
    vgfx_internal_set_mouse_position(
        window, vgfx_internal_scale_up_i32(x, cs), vgfx_internal_scale_up_i32(y, cs));

    extern void vgfx_platform_warp_cursor(vgfx_window_t w, int32_t x, int32_t y);
    vgfx_platform_warp_cursor(window, x, y);
}

/// @copydoc vgfx_set_relative_mouse
int32_t vgfx_set_relative_mouse(vgfx_window_t window, int32_t enabled) {
    if (!window)
        return 0;

    extern int vgfx_platform_set_relative_mouse(vgfx_window_t w, int enabled);
    int native = vgfx_platform_set_relative_mouse(window, enabled ? 1 : 0);

    vgfx_internal_event_lock(window);
    window->relative_mouse_enabled = enabled ? 1 : 0;
    window->relative_mouse_native = (enabled && native) ? 1 : 0;
    window->relative_dx_accum = 0.0;
    window->relative_dy_accum = 0.0;
    vgfx_internal_event_unlock(window);
    return native ? 1 : 0;
}

/// @copydoc vgfx_relative_mouse_native
int32_t vgfx_relative_mouse_native(vgfx_window_t window) {
    if (!window)
        return 0;
    vgfx_internal_event_lock(window);
    int32_t native = window->relative_mouse_enabled && window->relative_mouse_native;
    vgfx_internal_event_unlock(window);
    return native;
}

/// @copydoc vgfx_get_relative_deltas
void vgfx_get_relative_deltas(vgfx_window_t window, double *out_dx, double *out_dy) {
    double dx = 0.0;
    double dy = 0.0;
    if (window) {
        vgfx_internal_event_lock(window);
        dx = window->relative_dx_accum;
        dy = window->relative_dy_accum;
        window->relative_dx_accum = 0.0;
        window->relative_dy_accum = 0.0;
        vgfx_internal_event_unlock(window);
    }
    if (out_dx)
        *out_dx = dx;
    if (out_dy)
        *out_dy = dy;
}

/// @copydoc vgfx_hide_cursor
void vgfx_hide_cursor(void) {
    extern void vgfx_platform_hide_cursor(void);
    vgfx_platform_hide_cursor();
}

/// @copydoc vgfx_show_cursor
void vgfx_show_cursor(void) {
    extern void vgfx_platform_show_cursor(void);
    vgfx_platform_show_cursor();
}

//===----------------------------------------------------------------------===//
// Framebuffer Access
//===----------------------------------------------------------------------===//

/// @brief Get direct access to the window's framebuffer.
/// @details Returns a structure with pointers and dimensions for direct pixel
///          manipulation.  The framebuffer is in RGBA 8-8-8-8 format with
///          4 bytes per pixel (row-major, top-down).
///
/// @param window   Window handle
/// @param out_info Pointer to framebuffer info structure (filled on success)
/// @return 1 on success, 0 if window or out_info is NULL
///
/// @pre  window != NULL
/// @pre  out_info != NULL
/// @post If 1 returned: out_info filled with framebuffer details
///
/// @warning Direct framebuffer access bypasses bounds checking.  Incorrect
///          writes may corrupt memory.  Prefer vgfx_pset() for safety.
int32_t vgfx_get_framebuffer(vgfx_window_t window, vgfx_framebuffer_t *out_info) {
    if (!window || !out_info)
        return 0;

    vgfx_internal_event_lock(window);
    out_info->pixels = window->pixels;
    out_info->width = window->width;
    out_info->height = window->height;
    out_info->stride = window->stride;
    out_info->generation = window->framebuffer_generation;
    vgfx_internal_event_unlock(window);
    return 1;
}

/// @copydoc vgfx_set_gpu_present
void vgfx_set_gpu_present(vgfx_window_t window, int32_t enabled) {
    if (window)
        window->skip_software_present = enabled ? 1 : 0;
}

/// @copydoc vgfx_set_native_msg_hook
void vgfx_set_native_msg_hook(vgfx_window_t window, vgfx_native_msg_hook_t hook, void *user) {
    if (!window)
        return;
    window->native_msg_hook = hook;
    window->native_msg_hook_user = hook ? user : NULL;
}

//===----------------------------------------------------------------------===//
// End of Core Implementation
//===----------------------------------------------------------------------===//
