//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_videowidget.h
// Purpose: Public and app-scheduler contracts for the GUI VideoWidget, which
//          wraps VideoPlayer with transport controls, efficient frame upload,
//          timeline scrubbing, fullscreen state, and consumable media events.
//
// Key invariants:
//   - Owns a VideoPlayer internally for decode.
//   - App-owned widgets auto-update at most once per app frame generation.
//   - Manual Update remains compatible and is idempotent with auto-update.
//   - Decoded frame conversion storage is reused across steady-state frames.
//   - Slider widget shows playback progress.
//   - Play/Pause/Stop buttons delegate to VideoPlayer.
// Ownership/Lifetime:
//   - The VideoWidget owns its VideoPlayer and conversion buffer.
//   - Its retained widget subtree is owned by the parent GUI tree.
//   - App destruction invalidates registered wrappers before destroying that tree.
//
// Links: rt_videoplayer.h, rt_gui.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a video-playback widget under @p parent that loads the file at @p path.
/// @param parent Required live parent-container handle.
/// @param path Runtime string naming the video source.
/// @return New runtime-managed VideoWidget handle, or NULL if construction fails.
void *rt_videowidget_new(void *parent, rt_string path);
/// @brief Destroy the widget subtree and release the owned VideoPlayer immediately.
/// @details Safe to call multiple times; later calls are no-ops.
/// @param vw VideoWidget handle; invalid and already-disposed values are ignored.
void rt_videowidget_destroy(void *vw);
/// @brief Start or resume playback.
/// @param vw VideoWidget handle; invalid values are ignored.
void rt_videowidget_play(void *vw);
/// @brief Pause playback (current frame stays displayed).
/// @param vw VideoWidget handle; invalid values are ignored.
void rt_videowidget_pause(void *vw);
/// @brief Stop playback and rewind to the start.
/// @param vw VideoWidget handle; invalid values are ignored.
void rt_videowidget_stop(void *vw);
/// @brief Tick the underlying VideoPlayer by @p dt seconds and refresh the display image.
/// @details When automatic scheduling is enabled, repeated manual and app-scheduler calls within
///          one frame generation collapse to one decode/upload operation.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param dt Elapsed playback seconds.
void rt_videowidget_update(void *vw, double dt);

/// @brief Enable or disable app-frame scheduling for a VideoWidget.
/// @details Auto-update is enabled by default for widgets attached to an app. Disabling it keeps
///          all legacy manual Update behavior and removes playback deadlines from the app poller.
/// @param vw VideoWidget handle; invalid handles are ignored.
/// @param enabled Non-zero to update from the owning app's render scheduler.
void rt_videowidget_set_auto_update(void *vw, int64_t enabled);

/// @brief Report whether app-frame scheduling is enabled.
/// @param vw VideoWidget handle.
/// @return 1 when enabled on a live widget, otherwise 0.
int64_t rt_videowidget_is_auto_update(void *vw);

/// @brief Consume one successful-load event edge.
/// @details Construction records exactly one load edge after the player and retained subtree are
///          ready. Reading this edge does not consume other media events or change the revision.
/// @param vw VideoWidget handle.
/// @return 1 when an unread load edge existed, otherwise 0.
int64_t rt_videowidget_was_loaded(void *vw);

/// @brief Consume one frame-processing failure edge.
/// @details Failures are edge-triggered per failure episode; repeated frames with the same latched
///          failure do not flood the event queue. A later successful upload rearms the edge.
/// @param vw VideoWidget handle.
/// @return 1 when an unread failure edge existed, otherwise 0.
int64_t rt_videowidget_was_failed(void *vw);

/// @brief Consume one buffering-state transition edge.
/// @details The current implementation enters buffering when an actively playing player has no
///          decoded frame and leaves it when a frame becomes available or playback stops.
/// @param vw VideoWidget handle.
/// @return 1 for one unread enter/leave transition, otherwise 0.
int64_t rt_videowidget_was_buffering_changed(void *vw);

/// @brief Consume one natural-end event edge.
/// @details Looping still produces an end edge before playback restarts, permitting analytics and
///          UI state to observe each completed iteration.
/// @param vw VideoWidget handle.
/// @return 1 when an unread natural-end edge existed, otherwise 0.
int64_t rt_videowidget_was_ended(void *vw);

/// @brief Consume one timeline-seek event edge.
/// @param vw VideoWidget handle.
/// @return 1 when the position slider caused an unread seek, otherwise 0.
int64_t rt_videowidget_was_seeked(void *vw);

/// @brief Return the most recent frame-processing diagnostic.
/// @details A successful frame upload clears the diagnostic. The returned runtime string is owned
///          by the caller and is empty when no current failure exists.
/// @param vw VideoWidget handle.
/// @return Caller-owned runtime string containing the diagnostic or an empty string.
rt_string rt_videowidget_get_error(void *vw);

/// @brief Return the non-consuming VideoWidget state revision.
/// @details The counter advances for public configuration changes and media event transitions and
///          saturates at UINT64_MAX. Event consumers never modify it.
/// @param vw VideoWidget handle.
/// @return Monotonic revision, or zero for an invalid handle.
int64_t rt_videowidget_get_revision(void *vw);

/// @brief Configure deterministic transport-control auto-hide behavior.
/// @details When enabled, requested-visible controls hide after 2.5 seconds of uninterrupted
///          playback and reappear for transport actions, seeks, pause, stop, or end of stream.
/// @param vw VideoWidget handle; invalid handles are ignored.
/// @param enabled Non-zero to enable automatic hiding.
void rt_videowidget_set_controls_auto_hide(void *vw, int64_t enabled);

/// @brief Request fullscreen state on the VideoWidget's owning app window.
/// @details Unattached or invalid widgets are inert. Fullscreen state is app-scoped, so all
///          VideoWidgets in the same app observe the same result.
/// @param vw VideoWidget handle.
/// @param fullscreen Non-zero to enter fullscreen, zero to leave it.
void rt_videowidget_set_fullscreen(void *vw, int64_t fullscreen);

/// @brief Query fullscreen state from the owning app window.
/// @param vw VideoWidget handle.
/// @return 1 when the owning app is fullscreen, otherwise 0.
int64_t rt_videowidget_is_fullscreen(void *vw);
/// @brief Show or hide the play/pause/stop transport controls and timeline slider.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param show Non-zero to request visible controls.
void rt_videowidget_set_show_controls(void *vw, int8_t show);
/// @brief Return whether transport controls are configured to show.
/// @details Temporary auto-hide suppression does not change this requested state.
/// @param vw VideoWidget handle.
/// @return One when controls are configured to show, otherwise zero.
int64_t rt_videowidget_get_show_controls(void *vw);
/// @brief Toggle automatic restart when the video reaches its end.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param loop Non-zero to enable looping.
void rt_videowidget_set_loop(void *vw, int8_t loop);
/// @brief Return 1 when automatic looping is enabled.
/// @param vw VideoWidget handle.
/// @return One when looping is enabled, otherwise zero.
int64_t rt_videowidget_get_loop(void *vw);
/// @brief Set audio playback volume (0.0 = mute, 1.0 = full).
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param vol Requested gain, clamped to [0,1].
void rt_videowidget_set_volume(void *vw, double vol);
/// @brief 1 if currently playing, 0 if paused/stopped.
/// @param vw VideoWidget handle.
/// @return Underlying player's playing flag, or zero when unavailable.
int64_t rt_videowidget_get_is_playing(void *vw);
/// @brief Get the current playback position in seconds.
/// @param vw VideoWidget handle.
/// @return Current playback position, or zero when unavailable.
double rt_videowidget_get_position(void *vw);
/// @brief Get the total stream duration in seconds.
/// @param vw VideoWidget handle.
/// @return Stream duration, or zero when unavailable.
double rt_videowidget_get_duration(void *vw);
/// @brief Return the internal root widget so callers can compose or inspect layout.
/// @param vw VideoWidget handle.
/// @return Borrowed root-container handle, or NULL when unavailable.
void *rt_videowidget_get_root(void *vw);
// Proxy common Widget APIs through to the internal root widget so a VideoWidget
// can be laid out like any other widget.
/// @brief Show or hide the whole widget.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param visible Non-zero to show the retained subtree.
void rt_videowidget_set_visible(void *vw, int64_t visible);
/// @brief Enable or disable interaction with the widget.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param enabled Non-zero to enable the retained subtree.
void rt_videowidget_set_enabled(void *vw, int64_t enabled);
/// @brief Set the widget's fixed pixel size.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param width Requested width.
/// @param height Requested height.
void rt_videowidget_set_size(void *vw, int64_t width, int64_t height);
/// @brief Set the preferred (natural) size used during flex layout.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param width Preferred logical width.
/// @param height Preferred logical height.
void rt_videowidget_set_preferred_size(void *vw, double width, double height);
/// @brief Cap the widget's maximum size during layout.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param width Maximum logical width.
/// @param height Maximum logical height.
void rt_videowidget_set_max_size(void *vw, double width, double height);
/// @brief Set the flex grow factor for distributing extra space in a flex container.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param flex Requested flex-grow factor.
void rt_videowidget_set_flex(void *vw, double flex);
/// @brief Set the outer margin (in pixels) around the widget.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param margin Requested uniform margin.
void rt_videowidget_set_margin(void *vw, int64_t margin);
/// @brief Set the widget's position relative to its parent.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param x Parent-relative X coordinate.
/// @param y Parent-relative Y coordinate.
void rt_videowidget_set_position(void *vw, int64_t x, int64_t y);
/// @brief Append a child widget to the internal root container.
/// @param vw VideoWidget handle; invalid values are ignored.
/// @param child Child-widget handle forwarded to the generic tree API.
void rt_videowidget_add_child(void *vw, void *child);

/// @brief Update all automatically scheduled VideoWidgets owned by one app.
/// @details Called once by the GUI render scheduler after it advances @p frame_generation. A
///          manual update pending for the same generation is acknowledged without decoding again.
/// @param app Borrowed owning GUI app pointer.
/// @param dt Sanitized elapsed seconds for this scheduler frame.
/// @param frame_generation Non-zero monotonic app frame generation.
/// @internal
void rt_videowidget_update_app(void *app, double dt, uint64_t frame_generation);

/// @brief Return the nearest playback deadline for one app's auto-updated videos.
/// @param app Borrowed owning GUI app pointer.
/// @return 16 milliseconds while any owned widget is playing, otherwise -1.
/// @internal
int64_t rt_videowidget_next_deadline_ms(const void *app);

/// @brief Invalidate and release every VideoWidget controller owned by an app being destroyed.
/// @details Retained subtrees are left for the app root destruction pass; controller players and
///          conversion buffers are released and subsequent wrapper calls become inert.
/// @param app Borrowed app pointer whose lifetime is ending.
/// @internal
void rt_videowidget_forget_app(void *app);

#ifdef __cplusplus
}
#endif
