//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_videowidget.c
// Purpose: App-scheduled GUI video playback with reusable frame conversion,
//          transport controls, fullscreen integration, and consumable events.
//
// Key invariants:
//   - Video frames decoded by VideoPlayer are atomically uploaded to vg_image_t.
//   - Slider tracks playback position (0.0-1.0 normalized).
//   - Automatic and manual updates decode/upload at most once per app frame.
//   - Pixel format conversion: Zanna Pixels (0xRRGGBBAA) → vg_image RGBA bytes.
//   - Media event edges are independent; consuming one never consumes another.
// Ownership/Lifetime:
//   - The wrapper owns its player and reusable RGBA conversion buffer.
//   - The owning app's retained root owns the widget subtree.
//   - App destruction invalidates wrappers before destroying the retained tree.
//
// Links: rt_videowidget.h, rt_videoplayer.h, rt_gui.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements app-scheduled GUI video playback and transport controls.
 *
 * @details Managed wrappers own VideoPlayer instances and reusable RGBA
 *          conversion storage, coordinate manual and automatic updates by app
 *          generation, atomically upload decoded frames, drive controls and
 *          fullscreen state, and expose independent saturating media-event
 *          counters plus diagnostics.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_videowidget.h"
#include "rt_gui.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_videoplayer.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Decoder-aligned cap for one converted video frame.
#define RT_VIDEOWIDGET_MAX_FRAME_PIXELS ((size_t)64u * 1024u * 1024u)
/// @brief Maximum authenticated VideoWidget wrappers retained at once.
#define RT_VIDEOWIDGET_MAX_WRAPPERS ((size_t)1000000)
/// @brief Maximum lower-layer text scanned by an isolated VideoWidget export.
#define RT_VIDEOWIDGET_MAX_STRING_BYTES ((size_t)64u * 1024u * 1024u)

/// @copydoc rt_obj_new_i64()
extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
/// @copydoc rt_obj_set_finalizer()
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
/// @copydoc rt_obj_release_check0()
extern int rt_obj_release_check0(void *obj);
/// @copydoc rt_obj_free()
extern void rt_obj_free(void *obj);

/// @brief Convert a lower-layer VideoWidget C string without an unbounded scan.
/// @param text Borrowed candidate NUL-terminated text.
/// @return Owned runtime string, or the canonical empty string for invalid/oversized input.
static rt_string rt_videowidget_string_from_cstr(const char *text) {
    if (!text)
        return rt_string_from_bytes("", 0);
    size_t length = 0;
    while (length <= RT_VIDEOWIDGET_MAX_STRING_BYTES && text[length] != '\0')
        ++length;
    return length <= RT_VIDEOWIDGET_MAX_STRING_BYTES ? rt_string_from_bytes(text, length)
                                                     : rt_string_from_bytes("", 0);
}

/// @brief Normalize a non-negative media time read from the isolated player API.
/// @param value Candidate seconds value.
/// @return @p value when finite and non-negative, otherwise zero.
static double rt_videowidget_nonnegative_finite_or_zero(double value) {
    return isfinite(value) && value >= 0.0 ? value : 0.0;
}

/* GUI parent validation shim implemented by rt_gui_widgets.c. Kept as a
 * single external dependency so isolated VideoWidget contract tests can stub
 * the runtime GUI layer without linking all widget/app objects. */
/// @copydoc rt_gui_widget_parent_container_checked()
extern void *rt_gui_widget_parent_container_checked(void *handle);
/// @copydoc rt_gui_widget_owner_app()
extern void *rt_gui_widget_owner_app(void *handle);
/// @copydoc rt_gui_app_frame_generation_for_owner()
extern uint64_t rt_gui_app_frame_generation_for_owner(void *app);
/// @copydoc rt_gui_image_try_set_rgba_bytes()
extern int rt_gui_image_try_set_rgba_bytes(void *image,
                                           const uint8_t *rgba,
                                           int64_t width,
                                           int64_t height);

/// @copydoc rt_videoplayer_open()
extern void *rt_videoplayer_open(rt_string path);
/// @copydoc rt_videoplayer_play()
extern void rt_videoplayer_play(void *vp);
/// @copydoc rt_videoplayer_pause()
extern void rt_videoplayer_pause(void *vp);
/// @copydoc rt_videoplayer_stop()
extern void rt_videoplayer_stop(void *vp);
/// @copydoc rt_videoplayer_update()
extern void rt_videoplayer_update(void *vp, double dt);
/// @copydoc rt_videoplayer_seek()
extern void rt_videoplayer_seek(void *vp, double seconds);
/// @copydoc rt_videoplayer_set_volume()
extern void rt_videoplayer_set_volume(void *vp, double vol);
/// @copydoc rt_videoplayer_get_width()
extern int64_t rt_videoplayer_get_width(void *vp);
/// @copydoc rt_videoplayer_get_height()
extern int64_t rt_videoplayer_get_height(void *vp);
/// @copydoc rt_videoplayer_get_duration()
extern double rt_videoplayer_get_duration(void *vp);
/// @copydoc rt_videoplayer_get_position()
extern double rt_videoplayer_get_position(void *vp);
/// @copydoc rt_videoplayer_get_is_playing()
extern int64_t rt_videoplayer_get_is_playing(void *vp);
/// @copydoc rt_videoplayer_get_frame()
extern void *rt_videoplayer_get_frame(void *vp);

/// @brief Managed VideoWidget controller, retained subtree, and playback state.
typedef struct {
    /// @brief Live-wrapper authentication value.
    uint64_t magic;
    /// @brief Reserved runtime virtual-table slot.
    void *vptr;
    /* Owned components */
    /// @brief Owned VideoPlayer.
    void *player; /* rt_videoplayer */
    /// @brief Retained root VBox attached to the parent.
    void *root_widget; /* VBox container attached to parent */
    /// @brief Image widget displaying decoded frames.
    void *image_widget; /* vg_image_t for video display */
    /// @brief HBox containing transport controls.
    void *controls_widget; /* HBox container for transport controls */
    /// @brief Borrowed Play button in the owned subtree.
    void *play_button;
    /// @brief Borrowed Pause button in the owned subtree.
    void *pause_button;
    /// @brief Borrowed Stop button in the owned subtree.
    void *stop_button;
    /// @brief Borrowed normalized timeline slider.
    void *position_slider;
    /// @brief Borrowed owning GUI application.
    void *owner_app; /* borrowed rt_gui_app_t */
    /// @brief Owned reusable frame-conversion buffer.
    uint8_t *rgba_scratch;
    /// @brief Allocated bytes in @ref rgba_scratch.
    size_t rgba_scratch_capacity;
    /// @brief Borrowed most recently uploaded frame identity.
    const void *last_frame;
    /// @brief Borrowed stable current diagnostic literal.
    const char *error; /* borrowed stable diagnostic literal */
    /* Config */
    /// @brief Requested transport-control visibility.
    int8_t show_controls;
    /// @brief Whether natural end restarts playback.
    int8_t looping;
    /// @brief Whether the app scheduler drives updates.
    int8_t auto_update;
    /// @brief Whether controls hide during playback idle time.
    int8_t controls_auto_hide;
    /// @brief Current transient automatic-hide state.
    int8_t controls_hidden_by_auto;
    /// @brief Current playing-without-frame state.
    int8_t buffering;
    /// @brief Whether the natural end is currently latched.
    int8_t at_end;
    /// @brief Whether the current failure episode emitted an edge.
    int8_t failure_latched;
    /// @brief Manual update awaiting generation reconciliation.
    int8_t manual_update_pending;
    /// @brief Normalized audio gain.
    double volume;
    /// @brief Last timeline value synchronized with playback.
    double slider_last_value;
    /// @brief Continuous playback time since control activity.
    double controls_idle_seconds;
    /// @brief Position associated with @ref last_frame.
    double last_uploaded_position;
    /* Cached dimensions */
    /// @brief Cached decoded frame width.
    int32_t video_width;
    /// @brief Cached decoded frame height.
    int32_t video_height;
    /// @brief Saturating non-consuming state revision.
    uint64_t revision;
    /// @brief Unconsumed successful-load edges.
    uint64_t loaded_edges;
    /// @brief Unconsumed frame-failure edges.
    uint64_t failed_edges;
    /// @brief Unconsumed buffering-transition edges.
    uint64_t buffering_changed_edges;
    /// @brief Unconsumed natural-end edges.
    uint64_t ended_edges;
    /// @brief Unconsumed user-seek edges.
    uint64_t seeked_edges;
    /// @brief Last app generation processed automatically.
    uint64_t last_auto_generation;
} rt_videowidget;

/// @brief Release controller resources and optionally destroy its retained subtree.
/// @param w Candidate VideoWidget wrapper.
/// @param destroy_widget_tree Nonzero to destroy the root widget hierarchy.
static void videowidget_dispose(rt_videowidget *w, int destroy_widget_tree);

/// @brief Magic value authenticating live VideoWidget wrappers.
#define RT_VIDEOWIDGET_MAGIC UINT64_C(0x5254564944454F57)

/// @brief Global registry authenticating opaque VideoWidget handles.
static rt_videowidget **s_videowidget_wrappers = NULL;
/// @brief Number of live registered VideoWidget wrappers.
static size_t s_videowidget_wrapper_count = 0;
/// @brief Allocated wrapper-registry capacity.
static size_t s_videowidget_wrapper_cap = 0;

/// @brief Record a wrapper in the global VideoWidget registry (idempotent).
/// @details The registry is the source of truth for handle validation: videowidget_checked
///          only trusts an opaque `void*` once it is found here (then verifies the magic
///          tag), guarding against forged/freed handles. Capacity doubles from 8.
/// @param w Wrapper address to register; NULL is rejected.
/// @return 1 on success or if already present; 0 on overflow or realloc failure.
static int videowidget_register_wrapper(rt_videowidget *w) {
    if (!w)
        return 0;
    for (size_t i = 0; i < s_videowidget_wrapper_count; i++) {
        if (s_videowidget_wrappers[i] == w)
            return 1;
    }
    if (s_videowidget_wrapper_count >= s_videowidget_wrapper_cap) {
        if (s_videowidget_wrapper_count >= RT_VIDEOWIDGET_MAX_WRAPPERS)
            return 0;
        size_t new_cap = s_videowidget_wrapper_cap ? s_videowidget_wrapper_cap : 8;
        while (new_cap <= s_videowidget_wrapper_count) {
            if (new_cap > RT_VIDEOWIDGET_MAX_WRAPPERS / 2u) {
                new_cap = RT_VIDEOWIDGET_MAX_WRAPPERS;
                break;
            }
            new_cap *= 2u;
        }
        if (new_cap <= s_videowidget_wrapper_count || new_cap > SIZE_MAX / sizeof(rt_videowidget *))
            return 0;
        void *p = realloc(s_videowidget_wrappers, new_cap * sizeof(rt_videowidget *));
        if (!p)
            return 0;
        s_videowidget_wrappers = (rt_videowidget **)p;
        s_videowidget_wrapper_cap = new_cap;
    }
    s_videowidget_wrappers[s_videowidget_wrapper_count++] = w;
    return 1;
}

/// @brief Remove a wrapper from the VideoWidget registry, compacting the array. No-op if absent.
/// @param w Wrapper address to remove; NULL and unregistered addresses are ignored.
static void videowidget_unregister_wrapper(rt_videowidget *w) {
    if (!w)
        return;
    for (size_t i = 0; i < s_videowidget_wrapper_count; i++) {
        if (s_videowidget_wrappers[i] != w)
            continue;
        memmove(&s_videowidget_wrappers[i],
                &s_videowidget_wrappers[i + 1],
                (s_videowidget_wrapper_count - i - 1) * sizeof(*s_videowidget_wrappers));
        s_videowidget_wrapper_count--;
        if (s_videowidget_wrapper_count == 0) {
            free(s_videowidget_wrappers);
            s_videowidget_wrappers = NULL;
            s_videowidget_wrapper_cap = 0;
        }
        return;
    }
}

/// @brief True if @p w is a currently-registered wrapper; backs handle validation.
/// @param w Candidate wrapper address.
/// @return Non-zero only when the exact address occurs in the live registry.
static int videowidget_wrapper_is_registered(const rt_videowidget *w) {
    if (!w)
        return 0;
    for (size_t i = 0; i < s_videowidget_wrapper_count; i++) {
        if (s_videowidget_wrappers[i] == w)
            return 1;
    }
    return 0;
}

/// @brief Safe-cast an opaque handle to a VideoWidget: NULL unless it is a live
///        registered wrapper carrying the expected magic tag.
/// @param obj Candidate opaque VideoWidget handle.
/// @return Borrowed live registered wrapper, or NULL for invalid, forged, or disposed values.
static rt_videowidget *videowidget_checked(void *obj) {
    rt_videowidget *w = (rt_videowidget *)obj;
    return videowidget_wrapper_is_registered(w) && w->magic == RT_VIDEOWIDGET_MAGIC ? w : NULL;
}

/// @brief Advance the widget's saturating non-consuming state revision.
/// @param w Live widget to update.
static void videowidget_note_revision(rt_videowidget *w) {
    if (w && w->revision != UINT64_MAX)
        ++w->revision;
}

/// @brief Record one independent consumable media-event edge and state revision.
/// @param w Live widget receiving the event.
/// @param edges Saturating event counter owned by @p w.
static void videowidget_note_event(rt_videowidget *w, uint64_t *edges) {
    if (!w || !edges)
        return;
    if (*edges != UINT64_MAX)
        ++*edges;
    videowidget_note_revision(w);
}

/// @brief Consume at most one edge from a saturating media-event counter.
/// @param edges Event counter to inspect and decrement.
/// @return 1 when an edge was consumed, otherwise 0.
static int64_t videowidget_consume_event(uint64_t *edges) {
    if (!edges || *edges == 0u)
        return 0;
    --*edges;
    return 1;
}

/// @brief Set a frame-processing failure once per consecutive failure episode.
/// @details Repeated bad frames retain the diagnostic without flooding WasFailed. A successful
///          upload clears and rearms the latch.
/// @param w Live widget.
/// @param error Stable non-empty diagnostic literal.
static void videowidget_set_failure(rt_videowidget *w, const char *error) {
    if (!w || !error)
        return;
    w->error = error;
    if (!w->failure_latched) {
        w->failure_latched = 1;
        videowidget_note_event(w, &w->failed_edges);
    }
}

/// @brief Clear a recovered frame failure and rearm the next failure edge.
/// @param w Live widget whose upload just succeeded.
static void videowidget_clear_failure(rt_videowidget *w) {
    if (!w || (!w->failure_latched && !w->error))
        return;
    w->failure_latched = 0;
    w->error = NULL;
    videowidget_note_revision(w);
}

/// @brief Record a buffering-state transition without conflating it with failure.
/// @param w Live widget.
/// @param buffering Non-zero when actively playing without a decoded frame.
static void videowidget_set_buffering(rt_videowidget *w, int buffering) {
    if (!w)
        return;
    const int8_t normalized = buffering ? 1 : 0;
    if (w->buffering == normalized)
        return;
    w->buffering = normalized;
    videowidget_note_event(w, &w->buffering_changed_edges);
}

/// @brief Apply the effective transport-strip visibility.
/// @param w Live widget.
/// @param visible Non-zero to show the retained controls subtree.
static void videowidget_apply_controls_visibility(rt_videowidget *w, int visible) {
    if (w && w->controls_widget)
        rt_widget_set_visible(w->controls_widget, visible ? 1 : 0);
}

/// @brief Reveal requested controls and reset the deterministic idle timer.
/// @param w Live widget receiving transport interaction.
static void videowidget_wake_controls(rt_videowidget *w) {
    if (!w)
        return;
    w->controls_idle_seconds = 0.0;
    w->controls_hidden_by_auto = 0;
    videowidget_apply_controls_visibility(w, w->show_controls != 0);
}

/// @brief GC finalizer for a VideoWidget.
/// @details The wrapper owns the transport subtree it created. If user code
///          drops the wrapper while the GUI tree is still attached, destroy the
///          subtree so controls are not left orphaned with a released player.
/// @param obj Runtime-managed VideoWidget wrapper supplied by the object finalizer.
static void videowidget_finalizer(void *obj) {
    videowidget_dispose((rt_videowidget *)obj, 1);
}

/// @brief Release a GC-managed object if it exists, freeing it when the refcount drops to zero.
/// @param obj Runtime-managed object to release; NULL is ignored.
static void release_gc_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release all resources held by a VideoWidget, optionally destroying the GUI tree.
/// @details Clears all widget pointers (image, controls, buttons, slider) to prevent
///   dangling references after disposal. When @p destroy_widget_tree is non-zero it
///   also calls `rt_widget_destroy` on the root, which tears down the entire GUI
///   subtree — used during explicit `VideoWidget.Destroy` calls and GC finalization.
/// @param w Candidate live wrapper; invalid and already-disposed wrappers are ignored.
/// @param destroy_widget_tree Non-zero to destroy the retained root subtree before clearing it.
static void videowidget_dispose(rt_videowidget *w, int destroy_widget_tree) {
    if (!w || w->magic != RT_VIDEOWIDGET_MAGIC)
        return;
    videowidget_unregister_wrapper(w);

    if (destroy_widget_tree && w->root_widget) {
        rt_widget_destroy(w->root_widget);
    }

    w->root_widget = NULL;
    w->image_widget = NULL;
    w->controls_widget = NULL;
    w->play_button = NULL;
    w->pause_button = NULL;
    w->stop_button = NULL;
    w->position_slider = NULL;
    w->owner_app = NULL;
    free(w->rgba_scratch);
    w->rgba_scratch = NULL;
    w->rgba_scratch_capacity = 0u;
    w->last_frame = NULL;
    w->error = NULL;

    if (w->player) {
        release_gc_object(w->player);
        w->player = NULL;
    }
    w->magic = 0;
}

/// @brief Clamp a volume value to [0.0, 1.0] before passing it to the video player.
/// @details Values outside [0, 1] are undefined behavior in most audio backends.
///   Clamping here shields the player from invalid values supplied by user code.
/// @param vol Requested audio gain.
/// @return Finite value clamped to [0,1], with non-finite input mapped to zero.
static double clamp_volume(double vol) {
    if (!isfinite(vol))
        return 0.0;
    if (vol < 0.0)
        return 0.0;
    if (vol > 1.0)
        return 1.0;
    return vol;
}

/// @brief Grow the reusable packed-to-RGBA conversion buffer atomically.
/// @param w Live widget that owns the buffer.
/// @param width Positive frame width.
/// @param height Positive frame height.
/// @return 1 when sufficient storage is available, otherwise 0 with old storage retained.
static int videowidget_ensure_rgba_scratch(rt_videowidget *w, int64_t width, int64_t height) {
    if (!w || width <= 0 || height <= 0 || width > INT32_MAX || height > INT32_MAX) {
        return 0;
    }
    const size_t frame_width = (size_t)width;
    const size_t frame_height = (size_t)height;
    if (frame_width > SIZE_MAX / frame_height)
        return 0;
    const size_t pixels = frame_width * frame_height;
    if (pixels > RT_VIDEOWIDGET_MAX_FRAME_PIXELS || pixels > SIZE_MAX / 4u)
        return 0;
    const size_t required = pixels * 4u;
    if (required <= w->rgba_scratch_capacity && w->rgba_scratch)
        return 1;
    uint8_t *replacement = (uint8_t *)malloc(required);
    if (!replacement)
        return 0;
    free(w->rgba_scratch);
    w->rgba_scratch = replacement;
    w->rgba_scratch_capacity = required;
    return 1;
}

/// @brief Convert and upload the current decoded frame without steady-state allocation.
/// @details Frame dimensions, storage, and conversion capacity are validated before the atomic
///          Image upload. Paused frames at an unchanged position skip conversion entirely.
/// @param w Live VideoWidget.
/// @param force Non-zero when a seek/transport transition requires refresh at an equal position.
/// @return 1 when a frame is available and current, otherwise 0.
static int videowidget_upload_current_frame(rt_videowidget *w, int force) {
    if (!w || !w->player)
        return 0;
    void *frame = rt_videoplayer_get_frame(w->player);
    const int playing = rt_videoplayer_get_is_playing(w->player) != 0;
    if (!frame) {
        videowidget_set_buffering(w, playing);
        return 0;
    }
    videowidget_set_buffering(w, 0);

    const int64_t frame_width = rt_pixels_width(frame);
    const int64_t frame_height = rt_pixels_height(frame);
    const uint32_t *raw = rt_pixels_raw_buffer(frame);
    if (frame_width <= 0 || frame_height <= 0 || frame_width > INT32_MAX ||
        frame_height > INT32_MAX) {
        videowidget_set_failure(w, "Video frame dimensions are invalid");
        return 0;
    }
    if (!raw) {
        videowidget_set_failure(w, "Video frame pixel data is unavailable");
        return 0;
    }

    double position = rt_videoplayer_get_position(w->player);
    if (!isfinite(position))
        position = 0.0;
    if (!force && w->last_frame == frame && w->video_width == (int32_t)frame_width &&
        w->video_height == (int32_t)frame_height && w->last_uploaded_position == position) {
        videowidget_clear_failure(w);
        return 1;
    }
    if (!videowidget_ensure_rgba_scratch(w, frame_width, frame_height)) {
        videowidget_set_failure(w, "Video frame storage could not be allocated");
        return 0;
    }

    const size_t pixel_count = (size_t)frame_width * (size_t)frame_height;
    for (size_t index = 0; index < pixel_count; ++index) {
        const uint32_t packed = raw[index];
        const size_t output = index * 4u;
        w->rgba_scratch[output + 0u] = (uint8_t)((packed >> 24u) & 0xFFu);
        w->rgba_scratch[output + 1u] = (uint8_t)((packed >> 16u) & 0xFFu);
        w->rgba_scratch[output + 2u] = (uint8_t)((packed >> 8u) & 0xFFu);
        w->rgba_scratch[output + 3u] = (uint8_t)(packed & 0xFFu);
    }
    if (!w->image_widget || !rt_gui_image_try_set_rgba_bytes(
                                w->image_widget, w->rgba_scratch, frame_width, frame_height)) {
        videowidget_set_failure(w, "Video frame image upload failed");
        return 0;
    }

    if (w->video_width != (int32_t)frame_width || w->video_height != (int32_t)frame_height) {
        w->video_width = (int32_t)frame_width;
        w->video_height = (int32_t)frame_height;
        rt_widget_set_size(w->image_widget, frame_width, frame_height);
    }
    w->last_frame = frame;
    w->last_uploaded_position = position;
    videowidget_clear_failure(w);
    videowidget_note_revision(w);
    return 1;
}

/// @brief Perform one authoritative transport/decode/upload scheduler step.
/// @param w Live VideoWidget.
/// @param dt Elapsed seconds; invalid or non-positive values do not advance the player.
static void videowidget_update_core(rt_videowidget *w, double dt) {
    if (!w || !w->player)
        return;
    const double elapsed = isfinite(dt) && dt > 0.0 ? dt : 0.0;
    int interaction = 0;
    int force_frame = 0;

    if (w->play_button && rt_widget_was_clicked(w->play_button)) {
        rt_videoplayer_play(w->player);
        videowidget_note_revision(w);
        interaction = 1;
    }
    if (w->pause_button && rt_widget_was_clicked(w->pause_button)) {
        rt_videoplayer_pause(w->player);
        videowidget_note_revision(w);
        interaction = 1;
    }
    if (w->stop_button && rt_widget_was_clicked(w->stop_button)) {
        rt_videoplayer_stop(w->player);
        w->at_end = 0;
        videowidget_note_revision(w);
        interaction = 1;
        force_frame = 1;
    }
    if (interaction)
        videowidget_wake_controls(w);

    if (elapsed > 0.0)
        rt_videoplayer_update(w->player, elapsed);

    double duration = rt_videoplayer_get_duration(w->player);
    if (!isfinite(duration) || duration < 0.0)
        duration = 0.0;
    if (w->position_slider && duration > 0.0) {
        double slider_value = rt_slider_get_value(w->position_slider);
        if (!isfinite(slider_value))
            slider_value = 0.0;
        if (slider_value < 0.0)
            slider_value = 0.0;
        if (slider_value > 1.0)
            slider_value = 1.0;
        if (slider_value != w->slider_last_value) {
            rt_videoplayer_seek(w->player, slider_value * duration);
            w->slider_last_value = slider_value;
            videowidget_note_event(w, &w->seeked_edges);
            videowidget_wake_controls(w);
            interaction = 1;
            force_frame = 1;
        }
    }

    double position = rt_videoplayer_get_position(w->player);
    if (!isfinite(position) || position < 0.0)
        position = 0.0;
    const int ended_now =
        duration > 0.0 && !rt_videoplayer_get_is_playing(w->player) && position >= duration - 0.001;
    if (ended_now && !w->at_end) {
        w->at_end = 1;
        videowidget_note_event(w, &w->ended_edges);
        videowidget_wake_controls(w);
    } else if (!ended_now) {
        w->at_end = 0;
    }
    if (ended_now && w->looping) {
        rt_videoplayer_stop(w->player);
        rt_videoplayer_play(w->player);
        w->at_end = 0;
        force_frame = 1;
    }

    (void)videowidget_upload_current_frame(w, force_frame || elapsed > 0.0);

    if (w->position_slider && duration > 0.0) {
        double playback_value = rt_videoplayer_get_position(w->player) / duration;
        if (!isfinite(playback_value))
            playback_value = 0.0;
        if (playback_value < 0.0)
            playback_value = 0.0;
        if (playback_value > 1.0)
            playback_value = 1.0;
        rt_slider_set_value(w->position_slider, playback_value);
        w->slider_last_value = playback_value;
    }

    const int playing = rt_videoplayer_get_is_playing(w->player) != 0;
    if (!w->show_controls) {
        w->controls_hidden_by_auto = 0;
        videowidget_apply_controls_visibility(w, 0);
    } else if (!w->controls_auto_hide || !playing) {
        if (w->controls_hidden_by_auto) {
            w->controls_hidden_by_auto = 0;
            videowidget_note_revision(w);
        }
        w->controls_idle_seconds = 0.0;
        videowidget_apply_controls_visibility(w, 1);
    } else if (!interaction && elapsed > 0.0) {
        w->controls_idle_seconds += elapsed;
        if (w->controls_idle_seconds >= 2.5 && !w->controls_hidden_by_auto) {
            w->controls_hidden_by_auto = 1;
            videowidget_apply_controls_visibility(w, 0);
            videowidget_note_revision(w);
        }
    }
}

/// @brief Construct a video-playback GUI widget. Opens the file via `rt_videoplayer_open`,
/// builds a vbox containing an Image widget (for frames) plus an hbox of Play/Pause/Stop buttons
/// and a position-slider. Returns NULL if the file can't be opened or the video has zero
/// dimensions.
/// @param parent Required live parent-container handle.
/// @param path Runtime string naming the video source to open.
/// @return New runtime-managed VideoWidget wrapper, or NULL for invalid input or setup failure.
void *rt_videowidget_new(void *parent, rt_string path) {
    RT_ASSERT_MAIN_THREAD();
    if (!parent || !path)
        return NULL;
    void *parent_widget = rt_gui_widget_parent_container_checked(parent);
    if (!parent_widget)
        return NULL;

    /* Open video file */
    void *player = rt_videoplayer_open(path);
    if (!player)
        return NULL;

    int64_t vw64 = rt_videoplayer_get_width(player);
    int64_t vh64 = rt_videoplayer_get_height(player);
    if (vw64 <= 0 || vh64 <= 0 || vw64 > INT32_MAX || vh64 > INT32_MAX) {
        release_gc_object(player);
        return NULL;
    }
    int32_t vw = (int32_t)vw64;
    int32_t vh = (int32_t)vh64;

    /* Create widget */
    rt_videowidget *w = (rt_videowidget *)rt_obj_new_i64(0, (int64_t)sizeof(rt_videowidget));
    if (!w) {
        release_gc_object(player);
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->magic = RT_VIDEOWIDGET_MAGIC;
    w->player = player;
    w->video_width = vw;
    w->video_height = vh;
    w->owner_app = rt_gui_widget_owner_app(parent_widget);
    w->show_controls = 1;
    w->looping = 0;
    w->auto_update = 1;
    w->volume = 1.0;
    w->slider_last_value = 0.0;
    w->last_uploaded_position = NAN;
    w->revision = 1u;
    w->last_auto_generation = UINT64_MAX;
    if (!videowidget_register_wrapper(w)) {
        videowidget_dispose(w, 0);
        release_gc_object(w);
        return NULL;
    }

    w->root_widget = rt_vbox_new();
    if (!w->root_widget) {
        videowidget_dispose(w, 0);
        release_gc_object(w);
        return NULL;
    }
    rt_container_set_spacing(w->root_widget, 8.0);

    /* Create image widget for video display */
    w->image_widget = rt_image_new(w->root_widget);
    if (!w->image_widget) {
        videowidget_dispose(w, 1);
        release_gc_object(w);
        return NULL;
    }
    rt_image_set_scale_mode(w->image_widget, 1); /* VG_IMAGE_SCALE_FIT */
    rt_widget_set_size(w->image_widget, vw, vh);
    rt_widget_set_flex(w->image_widget, 1.0);

    w->controls_widget = rt_hbox_new();
    if (w->controls_widget) {
        rt_container_set_spacing(w->controls_widget, 8.0);
        w->play_button = rt_button_new(w->controls_widget, rt_const_cstr("Play"));
        w->pause_button = rt_button_new(w->controls_widget, rt_const_cstr("Pause"));
        w->stop_button = rt_button_new(w->controls_widget, rt_const_cstr("Stop"));
        w->position_slider = rt_slider_new(w->controls_widget, 1);
        if (w->position_slider) {
            rt_widget_set_flex(w->position_slider, 1.0);
            rt_slider_set_range(w->position_slider, 0.0, 1.0);
            rt_slider_set_value(w->position_slider, 0.0);
        }
    }

    rt_obj_set_finalizer(w, videowidget_finalizer);
    rt_widget_add_child(parent_widget, w->root_widget);
    if (w->controls_widget)
        rt_widget_add_child(w->root_widget, w->controls_widget);
    videowidget_apply_controls_visibility(w, 1);

    /* Display first frame */
    videowidget_update_core(w, 0.0);
    videowidget_note_event(w, &w->loaded_edges);

    return w;
}

/// @brief Destroy the video widget subtree and release the owned VideoPlayer.
/// @param obj VideoWidget handle; invalid and already-disposed values are ignored.
void rt_videowidget_destroy(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    videowidget_dispose(w, 1);
}

/// @brief Begin or resume video playback. Forwards to the underlying VideoPlayer.
/// @param obj VideoWidget handle; invalid or playerless wrappers are ignored.
void rt_videowidget_play(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    if (!w->player)
        return;
    rt_videoplayer_play(w->player);
    w->at_end = 0;
    videowidget_wake_controls(w);
    videowidget_note_revision(w);
}

/// @brief Pause playback (preserves position). Resume with `_play`.
/// @param obj VideoWidget handle; invalid or playerless wrappers are ignored.
void rt_videowidget_pause(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    if (!w->player)
        return;
    rt_videoplayer_pause(w->player);
    videowidget_wake_controls(w);
    videowidget_note_revision(w);
}

/// @brief Stop playback and rewind to position 0.
/// @param obj VideoWidget handle; invalid or playerless wrappers are ignored.
void rt_videowidget_stop(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    if (!w->player)
        return;
    rt_videoplayer_stop(w->player);
    w->at_end = 0;
    videowidget_wake_controls(w);
    videowidget_note_revision(w);
}

/// @brief Manually request one transport/decode/upload update.
/// @details With auto-update enabled, a completed scheduler update or earlier manual update in the
///          current app generation suppresses duplicate work. Manual-only mode always updates.
/// @param obj VideoWidget handle.
/// @param dt Elapsed seconds supplied to the underlying player update.
void rt_videowidget_update(void *obj, double dt) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w || !w->player)
        return;
    if (w->auto_update && w->owner_app) {
        const uint64_t generation = rt_gui_app_frame_generation_for_owner(w->owner_app);
        if (generation == 0u) {
            videowidget_update_core(w, dt);
            return;
        }
        if (w->manual_update_pending || w->last_auto_generation == generation) {
            return;
        }
        videowidget_update_core(w, dt);
        w->manual_update_pending = 1;
        return;
    }
    videowidget_update_core(w, dt);
}

/// @brief Enable or disable app-owned automatic frame scheduling.
/// @details Changing mode resets idempotence bookkeeping so the next selected update path performs
///          one complete step. Unattached widgets can retain the flag but have no app deadline.
/// @param obj VideoWidget handle; invalid values are ignored.
/// @param enabled Non-zero to participate in owning-app update scheduling.
void rt_videowidget_set_auto_update(void *obj, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    const int8_t normalized = enabled ? 1 : 0;
    if (w->auto_update == normalized)
        return;
    w->auto_update = normalized;
    w->manual_update_pending = 0;
    w->last_auto_generation = UINT64_MAX;
    videowidget_note_revision(w);
}

/// @brief Return whether this live widget participates in app scheduling.
/// @param obj VideoWidget handle.
/// @return One when automatic updates are enabled, otherwise zero.
int64_t rt_videowidget_is_auto_update(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w && w->auto_update ? 1 : 0;
}

/// @brief Consume one successful-load edge without changing other event counters.
/// @param obj VideoWidget handle.
/// @return One when an edge was consumed, otherwise zero.
int64_t rt_videowidget_was_loaded(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w ? videowidget_consume_event(&w->loaded_edges) : 0;
}

/// @brief Consume one frame-processing failure edge.
/// @param obj VideoWidget handle.
/// @return One when an edge was consumed, otherwise zero.
int64_t rt_videowidget_was_failed(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w ? videowidget_consume_event(&w->failed_edges) : 0;
}

/// @brief Consume one buffering-state transition edge.
/// @param obj VideoWidget handle.
/// @return One when an edge was consumed, otherwise zero.
int64_t rt_videowidget_was_buffering_changed(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w ? videowidget_consume_event(&w->buffering_changed_edges) : 0;
}

/// @brief Consume one natural-end event edge.
/// @param obj VideoWidget handle.
/// @return One when an edge was consumed, otherwise zero.
int64_t rt_videowidget_was_ended(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w ? videowidget_consume_event(&w->ended_edges) : 0;
}

/// @brief Consume one timeline-seek event edge.
/// @param obj VideoWidget handle.
/// @return One when an edge was consumed, otherwise zero.
int64_t rt_videowidget_was_seeked(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w ? videowidget_consume_event(&w->seeked_edges) : 0;
}

/// @brief Return a caller-owned copy of the current frame diagnostic.
/// @param obj VideoWidget handle.
/// @return Owned diagnostic string, or an owned empty string when no error is latched.
rt_string rt_videowidget_get_error(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    const char *error = w && w->error ? w->error : "";
    return rt_videowidget_string_from_cstr(error);
}

/// @brief Return the saturating non-consuming widget revision in signed runtime range.
/// @param obj VideoWidget handle.
/// @return Current revision saturated at `INT64_MAX`, or zero for an invalid handle.
int64_t rt_videowidget_get_revision(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return 0;
    return w->revision > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)w->revision;
}

/// @brief Enable or disable the deterministic 2.5-second transport auto-hide policy.
/// @param obj VideoWidget handle; invalid values are ignored.
/// @param enabled Non-zero to hide idle controls during playback.
void rt_videowidget_set_controls_auto_hide(void *obj, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    const int8_t normalized = enabled ? 1 : 0;
    if (w->controls_auto_hide == normalized)
        return;
    w->controls_auto_hide = normalized;
    videowidget_wake_controls(w);
    videowidget_note_revision(w);
}

/// @brief Forward fullscreen state to this widget's borrowed owning app.
/// @param obj VideoWidget handle.
/// @param fullscreen Non-zero to request fullscreen, zero to leave it.
void rt_videowidget_set_fullscreen(void *obj, int64_t fullscreen) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w || !w->owner_app)
        return;
    const int64_t requested = fullscreen ? 1 : 0;
    if (rt_app_is_fullscreen(w->owner_app) == requested)
        return;
    rt_app_set_fullscreen(w->owner_app, requested);
    videowidget_note_revision(w);
}

/// @brief Query fullscreen state from this widget's borrowed owning app.
/// @param obj VideoWidget handle.
/// @return One when the owning app is fullscreen, otherwise zero.
int64_t rt_videowidget_is_fullscreen(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w && w->owner_app && rt_app_is_fullscreen(w->owner_app) ? 1 : 0;
}

/// @brief Toggle visibility of the play/pause/stop/slider strip (image widget always visible).
/// @param obj VideoWidget handle; invalid values are ignored.
/// @param show Non-zero to show transport controls.
void rt_videowidget_set_show_controls(void *obj, int8_t show) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    const int8_t normalized = show ? 1 : 0;
    if (w->show_controls == normalized)
        return;
    w->show_controls = normalized;
    w->controls_hidden_by_auto = 0;
    w->controls_idle_seconds = 0.0;
    videowidget_apply_controls_visibility(w, normalized);
    videowidget_note_revision(w);
}

/// @brief Return the configured transport-control visibility.
/// @details Reports the requested state independently of temporary auto-hide suppression.
/// @param obj VideoWidget handle.
/// @return One when controls are configured to show, otherwise zero.
int64_t rt_videowidget_get_show_controls(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return 0;
    return w->show_controls ? 1 : 0;
}

/// @brief Enable auto-loop. When enabled, the widget restarts playback on natural end-of-video.
/// @param obj VideoWidget handle; invalid values are ignored.
/// @param loop Non-zero to enable looping.
void rt_videowidget_set_loop(void *obj, int8_t loop) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    const int8_t normalized = loop ? 1 : 0;
    if (w->looping == normalized)
        return;
    w->looping = normalized;
    videowidget_note_revision(w);
}

/// @brief Return whether natural end-of-video restarts playback.
/// @param obj VideoWidget handle.
/// @return One when looping is enabled, otherwise zero.
int64_t rt_videowidget_get_loop(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return 0;
    return w->looping ? 1 : 0;
}

/// @brief Set audio output level [0.0, 1.0]. Clamped via `clamp_volume`. Forwarded to player.
/// @param obj VideoWidget handle; invalid values are ignored.
/// @param vol Requested audio gain; non-finite values become zero.
void rt_videowidget_set_volume(void *obj, double vol) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return;
    vol = clamp_volume(vol);
    if (w->volume == vol)
        return;
    w->volume = vol;
    if (w->player)
        rt_videoplayer_set_volume(w->player, vol);
    videowidget_note_revision(w);
}

/// @brief Return 1 if the underlying VideoPlayer is currently playing, else 0.
/// @param obj VideoWidget handle.
/// @return Underlying player's playing flag, or zero for an invalid/playerless widget.
int64_t rt_videowidget_get_is_playing(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return 0;
    return w->player ? rt_videoplayer_get_is_playing(w->player) : 0;
}

/// @brief Current playback position in seconds (forwarded from the underlying VideoPlayer).
/// @param obj VideoWidget handle.
/// @return Current position in seconds, or zero for an invalid/playerless widget.
double rt_videowidget_get_position(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return 0.0;
    return w->player
               ? rt_videowidget_nonnegative_finite_or_zero(rt_videoplayer_get_position(w->player))
               : 0.0;
}

/// @brief Total video duration in seconds (forwarded from the underlying VideoPlayer).
/// @param obj VideoWidget handle.
/// @return Duration in seconds, or zero for an invalid/playerless widget.
double rt_videowidget_get_duration(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (!w)
        return 0.0;
    return w->player
               ? rt_videowidget_nonnegative_finite_or_zero(rt_videoplayer_get_duration(w->player))
               : 0.0;
}

/// @brief Run one automatic update for every VideoWidget owned by an app.
/// @details Registry traversal allocates nothing. A pending manual update is acknowledged, and a
///          repeated call with the same generation is ignored, guaranteeing one decode/upload per
///          app generation without background threads.
/// @param app Borrowed owning GUI application; NULL is ignored.
/// @param dt Elapsed seconds supplied to each player update.
/// @param frame_generation Non-zero owning-app frame generation used for idempotence.
void rt_videowidget_update_app(void *app, double dt, uint64_t frame_generation) {
    RT_ASSERT_MAIN_THREAD();
    if (!app || frame_generation == 0u)
        return;
    for (size_t index = 0; index < s_videowidget_wrapper_count; ++index) {
        rt_videowidget *w = s_videowidget_wrappers[index];
        if (!w || w->magic != RT_VIDEOWIDGET_MAGIC || w->owner_app != app || !w->auto_update ||
            !w->player) {
            continue;
        }
        if (w->last_auto_generation == frame_generation)
            continue;
        if (w->manual_update_pending) {
            w->manual_update_pending = 0;
            w->last_auto_generation = frame_generation;
            continue;
        }
        videowidget_update_core(w, dt);
        w->last_auto_generation = frame_generation;
    }
}

/// @brief Return a 16ms scheduler deadline while an owned auto-updated video is playing.
/// @param app Borrowed GUI application whose widgets are queried.
/// @return 16 when an owned video requires animation updates, otherwise -1.
int64_t rt_videowidget_next_deadline_ms(const void *app) {
    if (!app)
        return -1;
    for (size_t index = 0; index < s_videowidget_wrapper_count; ++index) {
        rt_videowidget *w = s_videowidget_wrappers[index];
        if (w && w->magic == RT_VIDEOWIDGET_MAGIC && w->owner_app == app && w->auto_update &&
            w->player && rt_videoplayer_get_is_playing(w->player)) {
            return 16;
        }
    }
    return -1;
}

/// @brief Release controller resources for every VideoWidget belonging to a dying app.
/// @details Subtrees are deliberately not destroyed here because app root teardown owns them. The
///          registry compacts during disposal, so the loop advances only past non-matching entries.
/// @param app Borrowed GUI application being destroyed; NULL is ignored.
void rt_videowidget_forget_app(void *app) {
    RT_ASSERT_MAIN_THREAD();
    if (!app)
        return;
    size_t index = 0u;
    while (index < s_videowidget_wrapper_count) {
        rt_videowidget *w = s_videowidget_wrappers[index];
        if (w && w->magic == RT_VIDEOWIDGET_MAGIC && w->owner_app == app) {
            videowidget_dispose(w, 0);
            continue;
        }
        ++index;
    }
}

/// @brief Return the VideoWidget's retained root container.
/// @param obj VideoWidget handle.
/// @return Borrowed root widget, or NULL for an invalid or disposed wrapper.
void *rt_videowidget_get_root(void *obj) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    return w ? w->root_widget : NULL;
}

/// @brief Forward visibility to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param visible Non-zero to show the complete widget subtree.
void rt_videowidget_set_visible(void *obj, int64_t visible) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_visible(w->root_widget, visible);
}

/// @brief Forward enabled state to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param enabled Non-zero to enable the complete widget subtree.
void rt_videowidget_set_enabled(void *obj, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_enabled(w->root_widget, enabled);
}

/// @brief Forward fixed integer dimensions to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param width Requested width.
/// @param height Requested height.
void rt_videowidget_set_size(void *obj, int64_t width, int64_t height) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_size(w->root_widget, width, height);
}

/// @brief Forward preferred logical dimensions to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param width Preferred logical width.
/// @param height Preferred logical height.
void rt_videowidget_set_preferred_size(void *obj, double width, double height) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_preferred_size(w->root_widget, width, height);
}

/// @brief Forward maximum logical dimensions to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param width Maximum logical width.
/// @param height Maximum logical height.
void rt_videowidget_set_max_size(void *obj, double width, double height) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_max_size(w->root_widget, width, height);
}

/// @brief Forward a flex-grow factor to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param flex Requested flex-grow factor.
void rt_videowidget_set_flex(void *obj, double flex) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_flex(w->root_widget, flex);
}

/// @brief Forward a uniform margin to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param margin Requested margin.
void rt_videowidget_set_margin(void *obj, int64_t margin) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_margin(w->root_widget, margin);
}

/// @brief Forward a manual position to the VideoWidget's retained root.
/// @param obj VideoWidget handle.
/// @param x Parent-relative X coordinate.
/// @param y Parent-relative Y coordinate.
void rt_videowidget_set_position(void *obj, int64_t x, int64_t y) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_set_position(w->root_widget, x, y);
}

/// @brief Attach a child widget to the VideoWidget's retained root container.
/// @param obj VideoWidget handle.
/// @param child Child-widget handle forwarded to the generic GUI tree API.
void rt_videowidget_add_child(void *obj, void *child) {
    RT_ASSERT_MAIN_THREAD();
    rt_videowidget *w = videowidget_checked(obj);
    if (w && w->root_widget)
        rt_widget_add_child(w->root_widget, child);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
