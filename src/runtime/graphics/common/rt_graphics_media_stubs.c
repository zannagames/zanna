//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics_media_stubs.c
/// @brief Graphics-disabled spatial audio, video player, and video widget
/// entry points.
///
/// @details This split source keeps media-related unavailable-backend symbols
/// together while preserving the original no-resource-allocation stub behavior.
///
// File: src/runtime/graphics/common/rt_graphics_media_stubs.c
// Purpose: Graphics-disabled spatial audio, video player, and video widget entry points.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"

/* Object-backed spatial-audio stubs */

/// @brief Stub for `SoundListener3D.New` — would normally create a
///        spatial-audio listener (the "ear" the mixer applies pan and
///        distance attenuation against). Typically one per scene.
///
/// Silent stub returning NULL.
///
/// @return `NULL`.
void *rt_soundlistener3d_new(void) {
    return NULL;
}

/// @brief Stub for `SoundListener3D.Position` — get the listener's
///        current world-space position as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param l SoundListener3D handle (ignored).
///
/// @return `NULL`.
void *rt_soundlistener3d_get_position(void *l) {
    (void)l;
    return NULL;
}

/// @brief Stub for `SoundListener3D.Position` setter — set the listener's
///        world-space position from a Vec3 handle.
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param p Vec3 position handle (ignored).
void rt_soundlistener3d_set_position(void *l, void *p) {
    (void)l;
    (void)p;
}

/// @brief Stub for `SoundListener3D.Position` XYZ setter — set the listener's
///        world-space position from raw doubles. Convenience overload.
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param x World-space x (ignored).
/// @param y World-space y (ignored).
/// @param z World-space z (ignored).
void rt_soundlistener3d_set_position_vec(void *l, double x, double y, double z) {
    (void)l;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `SoundListener3D.Forward` — get the listener's
///        forward-facing direction vector (used for stereo panning math).
///
/// Silent stub returning NULL.
///
/// @param l SoundListener3D handle (ignored).
///
/// @return `NULL`.
void *rt_soundlistener3d_get_forward(void *l) {
    (void)l;
    return NULL;
}

/// @brief Stub for `SoundListener3D.Forward` setter — set the listener's
///        forward direction (must be normalized).
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param f Vec3 forward direction (ignored).
void rt_soundlistener3d_set_forward(void *l, void *f) {
    (void)l;
    (void)f;
}

/// @brief Return no listener up vector in a graphics-disabled build.
/// @param l SoundListener3D handle (ignored).
/// @return `NULL`.
void *rt_soundlistener3d_get_up(void *l) {
    (void)l;
    return NULL;
}

/// @brief Ignore a listener up-vector update.
/// @param l SoundListener3D handle (ignored).
/// @param u Vec3 up direction (ignored).
void rt_soundlistener3d_set_up(void *l, void *u) {
    (void)l;
    (void)u;
}

/// @brief Stub for `SoundListener3D.Velocity` — get the listener's
///        velocity vector (used by the mixer for Doppler-effect math).
///
/// Silent stub returning NULL.
///
/// @param l SoundListener3D handle (ignored).
///
/// @return `NULL`.
void *rt_soundlistener3d_get_velocity(void *l) {
    (void)l;
    return NULL;
}

/// @brief Stub for `SoundListener3D.Velocity` setter — set the listener's
///        velocity vector for Doppler computation.
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param v Vec3 velocity handle (ignored).
void rt_soundlistener3d_set_velocity(void *l, void *v) {
    (void)l;
    (void)v;
}

/// @brief Stub for `SoundListener3D.IsActive` — true when this listener
///        is the currently-selected listener for the audio mixer
///        (multiple listeners may exist; only one is active at a time).
///
/// Silent stub returning `0`.
///
/// @param l SoundListener3D handle (ignored).
///
/// @return `0`.
int8_t rt_soundlistener3d_get_is_active(void *l) {
    (void)l;
    return 0;
}

/// @brief Stub for `SoundListener3D.SetActive` — designate this
///        listener as the active one (deactivates any previously-active
///        listener).
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param a Non-zero to make this listener active (ignored).
void rt_soundlistener3d_set_is_active(void *l, int8_t a) {
    (void)l;
    (void)a;
}

/// @brief Stub for `SoundListener3D.BindNode` — attach a SceneNode3D so
///        the listener's position/orientation follow the node each frame.
///        Use `BindCamera` for the typical first-person setup.
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param n SceneNode3D handle, or NULL to detach (ignored).
void rt_soundlistener3d_bind_node(void *l, void *n) {
    (void)l;
    (void)n;
}

/// @brief Stub for `SoundListener3D.ClearNodeBinding` — detach the
///        SceneNode3D currently driving the listener's spatial position.
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
void rt_soundlistener3d_clear_node_binding(void *l) {
    (void)l;
}

/// @brief Stub for `SoundListener3D.BindCamera` — attach a Camera3D so
///        the listener's position and forward vector follow the camera
///        each frame (typical first-person audio setup).
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
/// @param c Camera3D handle, or NULL to detach (ignored).
void rt_soundlistener3d_bind_camera(void *l, void *c) {
    (void)l;
    (void)c;
}

/// @brief Stub for `SoundListener3D.ClearCameraBinding` — detach the
///        Camera3D currently driving the listener.
///
/// Silent no-op stub.
///
/// @param l SoundListener3D handle (ignored).
void rt_soundlistener3d_clear_camera_binding(void *l) {
    (void)l;
}

/// @brief Stub for `SoundSource3D.New` — would normally create a 3D
///        positional emitter for the given Sound resource.
///
/// Silent stub returning NULL.
///
/// @param s Sound handle (ignored).
///
/// @return `NULL`.
void *rt_soundsource3d_new(void *s) {
    (void)s;
    return NULL;
}

/// @brief Stub for `SoundSource3D.Position` — get the source's current
///        world-space position as a Vec3.
///
/// Silent stub returning NULL.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `NULL`.
void *rt_soundsource3d_get_position(void *s) {
    (void)s;
    return NULL;
}

/// @brief Stub for `SoundSource3D.Position` setter — set the source's
///        world-space position from a Vec3 handle.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param p Vec3 position handle (ignored).
void rt_soundsource3d_set_position(void *s, void *p) {
    (void)s;
    (void)p;
}

/// @brief Stub for `SoundSource3D.Position` XYZ setter — set the source's
///        world-space position from raw doubles. Convenience overload.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param x World-space x (ignored).
/// @param y World-space y (ignored).
/// @param z World-space z (ignored).
void rt_soundsource3d_set_position_vec(void *s, double x, double y, double z) {
    (void)s;
    (void)x;
    (void)y;
    (void)z;
}

/// @brief Stub for `SoundSource3D.Velocity` — get the source's current
///        velocity vector. Used by the mixer for Doppler-effect calculations.
///
/// Silent stub returning NULL.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `NULL`.
void *rt_soundsource3d_get_velocity(void *s) {
    (void)s;
    return NULL;
}

/// @brief Stub for `SoundSource3D.Velocity` setter — set the source's
///        velocity vector for Doppler computation.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param v Vec3 velocity handle (ignored).
void rt_soundsource3d_set_velocity(void *s, void *v) {
    (void)s;
    (void)v;
}

/// @brief Return the neutral Doppler multiplier for a disabled sound source.
/// @param s SoundSource3D handle (ignored).
/// @return `1.0`.
double rt_soundsource3d_get_doppler_factor(void *s) {
    (void)s;
    return 1.0;
}

/// @brief Stub for `SoundSource3D.MaxDistance` — get the cutoff distance
///        beyond which the source is silent (inverse-distance attenuation
///        upper bound).
///
/// Silent stub returning `0.0`.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `0.0`.
double rt_soundsource3d_get_max_distance(void *s) {
    (void)s;
    return 0.0;
}

/// @brief Stub for `SoundSource3D.MaxDistance` setter — set the cutoff
///        distance for attenuation.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param d Max distance in world units (ignored).
void rt_soundsource3d_set_max_distance(void *s, double d) {
    (void)s;
    (void)d;
}

/// @brief Stub for `SoundSource3D.RefDistance` — get the full-volume radius.
/// @param s SoundSource3D handle (ignored).
/// @return `0.0`.
double rt_soundsource3d_get_ref_distance(void *s) {
    (void)s;
    return 0.0;
}

/// @brief Stub for `SoundSource3D.RefDistance` setter — set the full-volume radius.
/// @param s SoundSource3D handle (ignored).
/// @param d Reference distance in world units (ignored).
void rt_soundsource3d_set_ref_distance(void *s, double d) {
    (void)s;
    (void)d;
}

/// @brief Stub for `SoundSource3D.Volume` — get the per-source gain
///        multiplier (0..100, applied after distance attenuation).
///
/// Silent stub returning `0`.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `0`.
int64_t rt_soundsource3d_get_volume(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `SoundSource3D.Volume` setter — set the per-source gain
///        multiplier.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param v Volume 0..100 (ignored).
void rt_soundsource3d_set_volume(void *s, int64_t v) {
    (void)s;
    (void)v;
}

/// @brief Stub for `SoundSource3D.Pitch` getter — playback-rate multiplier.
///
/// Silent stub returning the native rate.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `1.0`.
double rt_soundsource3d_get_pitch(void *s) {
    (void)s;
    return 1.0;
}

/// @brief Stub for `SoundSource3D.Pitch` setter. Silent no-op.
/// @param s SoundSource3D handle (ignored).
/// @param pitch Playback-rate multiplier (ignored).
void rt_soundsource3d_set_pitch(void *s, double pitch) {
    (void)s;
    (void)pitch;
}

/// @brief Stub for `SoundSource3D.Occlusion` getter. Returns fully open.
/// @param s SoundSource3D handle (ignored).
/// @return `0.0`, indicating no occlusion.
double rt_soundsource3d_get_occlusion(void *s) {
    (void)s;
    return 0.0;
}

/// @brief Stub for `SoundSource3D.Occlusion` setter. Silent no-op.
/// @param s SoundSource3D handle (ignored).
/// @param amount Occlusion amount (ignored).
void rt_soundsource3d_set_occlusion(void *s, double amount) {
    (void)s;
    (void)amount;
}

/// @brief Silent stub: SoundSource3D mix-group routing is a no-op without graphics.
/// @param s SoundSource3D handle (ignored).
/// @param group Mixer group identifier (ignored).
void rt_soundsource3d_set_mix_group(void *s, int64_t group) {
    (void)s;
    (void)group;
}

/// @brief Silent stub: reports the default SFX group without graphics.
/// @param s SoundSource3D handle (ignored).
/// @return `1`, the default SFX mix-group identifier.
int64_t rt_soundsource3d_get_mix_group(void *s) {
    (void)s;
    return 1;
}

/// @brief Stub for `SoundSource3D.Looping` — get the looping flag.
///
/// Silent stub returning `0`.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `0`.
int8_t rt_soundsource3d_get_looping(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `SoundSource3D.Looping` setter — when enabled, the
///        underlying voice loops at the end of the sound buffer.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param l Non-zero to enable looping (ignored).
void rt_soundsource3d_set_looping(void *s, int8_t l) {
    (void)s;
    (void)l;
}

/// @brief Stub for `SoundSource3D.IsPlaying` — true while the active
///        voice for this source is producing audio.
///
/// Silent stub returning `0`.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `0`.
int8_t rt_soundsource3d_get_is_playing(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `SoundSource3D.VoiceId` — get the underlying mixer
///        voice handle (`0` if not playing). Useful for debugging.
///
/// Silent stub returning `0`.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `0`.
int64_t rt_soundsource3d_get_voice_id(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `SoundSource3D.Play` — start playback of the bound
///        Sound at the source's current position. Returns the assigned
///        voice id, or `0` on failure.
///
/// Silent stub returning `0`.
///
/// @param s SoundSource3D handle (ignored).
///
/// @return `0`.
int64_t rt_soundsource3d_play(void *s) {
    (void)s;
    return 0;
}

/// @brief Stub for `SoundSource3D.Stop` — halt the source's current
///        voice immediately. Safe no-op when not playing.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
void rt_soundsource3d_stop(void *s) {
    (void)s;
}

/// @brief Stub for `SoundSource3D.BindNode` — attach a SceneNode3D so
///        the source's position follows the node each frame
///        (`SpatialAudio3D.SyncBindings(dt)`).
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
/// @param n SceneNode3D handle, or NULL to detach (ignored).
void rt_soundsource3d_bind_node(void *s, void *n) {
    (void)s;
    (void)n;
}

/// @brief Stub for `SoundSource3D.ClearNodeBinding` — detach the
///        SceneNode3D currently driving the source's position.
///
/// Silent no-op stub.
///
/// @param s SoundSource3D handle (ignored).
void rt_soundsource3d_clear_node_binding(void *s) {
    (void)s;
}

/* VideoWidget stubs */

/// @brief Stub for `VideoWidget.New` — would normally create a GUI
///        Image widget bound to a VideoPlayer that decodes the file at
///        `path`. The widget's pixels are refreshed each `Update` call
///        with the latest decoded video frame.
///
/// Silent stub returning NULL.
///
/// @param p    Parent widget handle (ignored).
/// @param path Filesystem path to the video file (ignored).
///
/// @return `NULL`.
void *rt_videowidget_new(void *p, rt_string path) {
    (void)p;
    (void)path;
    return NULL;
}

/// @brief Stub for `VideoWidget.Destroy` — would normally destroy the
///        widget subtree and release the owned VideoPlayer immediately.
///
/// Silent no-op stub.
///
/// @param v VideoWidget handle (ignored).
void rt_videowidget_destroy(void *v) {
    (void)v;
}

/// @brief Stub for `VideoWidget.Play` — start or resume video playback
///        (delegates to the embedded VideoPlayer).
///
/// Silent no-op stub.
///
/// @param v VideoWidget handle (ignored).
void rt_videowidget_play(void *v) {
    (void)v;
}

/// @brief Stub for `VideoWidget.Pause` — pause playback. The widget
///        continues to display the current frame.
///
/// Silent no-op stub.
///
/// @param v VideoWidget handle (ignored).
void rt_videowidget_pause(void *v) {
    (void)v;
}

/// @brief Stub for `VideoWidget.Stop` — stop playback and rewind to
///        frame 0. The widget displays the first frame (or a placeholder
///        if no frame decoded yet).
///
/// Silent no-op stub.
///
/// @param v VideoWidget handle (ignored).
void rt_videowidget_stop(void *v) {
    (void)v;
}

/// @brief Stub for `VideoWidget.Update` — advance video playback by
///        `dt` seconds and refresh the widget's image. Should be called
///        once per GUI frame (typically from the app's frame callback).
///
/// Silent no-op stub.
///
/// @param v  VideoWidget handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_videowidget_update(void *v, double dt) {
    (void)v;
    (void)dt;
}

/// @brief Stub: graphics disabled — automatic VideoWidget scheduling is unavailable.
/// @param v VideoWidget handle (ignored).
/// @param enabled Non-zero to request automatic updates (ignored).
void rt_videowidget_set_auto_update(void *v, int64_t enabled) {
    (void)v;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no VideoWidget can be auto-updated.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_is_auto_update(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — no successful-load edge exists.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_was_loaded(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — no live controller can emit a failure edge.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_was_failed(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — no buffering transition exists.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_was_buffering_changed(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — no playback can reach end of stream.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_was_ended(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — no timeline seek can occur.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_was_seeked(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: return the stable graphics-disabled VideoWidget diagnostic.
/// @param v VideoWidget handle (ignored).
/// @return A constant runtime string explaining that GUI support is unavailable.
rt_string rt_videowidget_get_error(void *v) {
    (void)v;
    return rt_const_cstr("GUI support is not available in this build");
}

/// @brief Stub: graphics disabled — no VideoWidget revision exists.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_get_revision(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — transport controls cannot auto-hide.
/// @param v VideoWidget handle (ignored).
/// @param enabled Non-zero to enable auto-hide (ignored).
void rt_videowidget_set_controls_auto_hide(void *v, int64_t enabled) {
    (void)v;
    (void)enabled;
}

/// @brief Stub: graphics disabled — no owning window can enter fullscreen.
/// @param v VideoWidget handle (ignored).
/// @param fullscreen Non-zero to request fullscreen playback (ignored).
void rt_videowidget_set_fullscreen(void *v, int64_t fullscreen) {
    (void)v;
    (void)fullscreen;
}

/// @brief Stub: graphics disabled — VideoWidget fullscreen is always false.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_is_fullscreen(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub: graphics disabled — no app-owned videos require scheduler work.
/// @param app GUI application handle (ignored).
/// @param dt Frame interval in seconds (ignored).
/// @param frame_generation Monotonic GUI frame generation (ignored).
void rt_videowidget_update_app(void *app, double dt, uint64_t frame_generation) {
    (void)app;
    (void)dt;
    (void)frame_generation;
}

/// @brief Stub: graphics disabled — there is no VideoWidget scheduler deadline.
/// @param app GUI application handle (ignored).
/// @return `-1`, indicating no pending deadline.
int64_t rt_videowidget_next_deadline_ms(const void *app) {
    (void)app;
    return -1;
}

/// @brief Stub: graphics disabled — no VideoWidget controllers are app-owned.
/// @param app GUI application handle (ignored).
void rt_videowidget_forget_app(void *app) {
    (void)app;
}

/// @brief Stub for `VideoWidget.SetShowControls` — toggle the overlay
///        playback controls (play/pause button, scrubber bar) inside
///        the widget bounds.
///
/// Silent no-op stub.
///
/// @param v VideoWidget handle (ignored).
/// @param s Non-zero to show controls (ignored).
void rt_videowidget_set_show_controls(void *v, int8_t s) {
    (void)v;
    (void)s;
}

/// @brief Return the fallback controls-visibility flag.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_get_show_controls(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub for `VideoWidget.SetLoop` — when enabled, the widget
///        seeks back to frame 0 on EOF and continues playback (no
///        explicit `Play` call needed).
///
/// Silent no-op stub.
///
/// @param v VideoWidget handle (ignored).
/// @param l Non-zero to enable looping (ignored).
void rt_videowidget_set_loop(void *v, int8_t l) {
    (void)v;
    (void)l;
}

/// @brief Return the fallback playback-loop flag.
/// @param v VideoWidget handle (ignored).
/// @return `0`.
int64_t rt_videowidget_get_loop(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub for `VideoWidget.SetVolume` — set the audio track's
///        playback volume, 0..1. Delegates to the embedded VideoPlayer.
///
/// Silent no-op stub.
///
/// @param v   VideoWidget handle (ignored).
/// @param vol Volume, 0..1 (ignored).
void rt_videowidget_set_volume(void *v, double vol) {
    (void)v;
    (void)vol;
}

/// @brief Stub for `VideoWidget.IsPlaying` — true while the widget's
///        embedded VideoPlayer is actively decoding frames.
///
/// Silent stub returning `0`.
///
/// @param v VideoWidget handle (ignored).
///
/// @return `0`.
int64_t rt_videowidget_get_is_playing(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub for `VideoWidget.Position` — get the current playback
///        position in seconds.
///
/// Silent stub returning `0.0`.
///
/// @param v VideoWidget handle (ignored).
///
/// @return `0.0`.
double rt_videowidget_get_position(void *v) {
    (void)v;
    return 0.0;
}

/// @brief Stub for `VideoWidget.Duration` — get the total length of the
///        loaded video in seconds.
///
/// Silent stub returning `0.0`.
///
/// @param v VideoWidget handle (ignored).
///
/// @return `0.0`.
double rt_videowidget_get_duration(void *v) {
    (void)v;
    return 0.0;
}

/// @brief Return no root widget for an unavailable VideoWidget.
/// @param v VideoWidget handle (ignored).
/// @return `NULL`.
void *rt_videowidget_get_root(void *v) {
    (void)v;
    return NULL;
}

/// @brief Ignore a VideoWidget visibility update.
/// @param v VideoWidget handle (ignored).
/// @param visible Requested visibility flag (ignored).
void rt_videowidget_set_visible(void *v, int64_t visible) {
    (void)v;
    (void)visible;
}

/// @brief Ignore a VideoWidget enabled-state update.
/// @param v VideoWidget handle (ignored).
/// @param enabled Requested enabled flag (ignored).
void rt_videowidget_set_enabled(void *v, int64_t enabled) {
    (void)v;
    (void)enabled;
}

/// @brief Ignore a VideoWidget size update.
/// @param v VideoWidget handle (ignored).
/// @param width Width in logical pixels (ignored).
/// @param height Height in logical pixels (ignored).
void rt_videowidget_set_size(void *v, int64_t width, int64_t height) {
    (void)v;
    (void)width;
    (void)height;
}

/// @brief Ignore a VideoWidget preferred-size update.
/// @param v VideoWidget handle (ignored).
/// @param width Preferred width (ignored).
/// @param height Preferred height (ignored).
void rt_videowidget_set_preferred_size(void *v, double width, double height) {
    (void)v;
    (void)width;
    (void)height;
}

/// @brief Ignore a VideoWidget maximum-size update.
/// @param v VideoWidget handle (ignored).
/// @param width Maximum width (ignored).
/// @param height Maximum height (ignored).
void rt_videowidget_set_max_size(void *v, double width, double height) {
    (void)v;
    (void)width;
    (void)height;
}

/// @brief Ignore a VideoWidget flex-weight update.
/// @param v VideoWidget handle (ignored).
/// @param flex Layout flex weight (ignored).
void rt_videowidget_set_flex(void *v, double flex) {
    (void)v;
    (void)flex;
}

/// @brief Ignore a VideoWidget margin update.
/// @param v VideoWidget handle (ignored).
/// @param margin Uniform margin in logical pixels (ignored).
void rt_videowidget_set_margin(void *v, int64_t margin) {
    (void)v;
    (void)margin;
}

/// @brief Ignore a VideoWidget position update.
/// @param v VideoWidget handle (ignored).
/// @param x Left coordinate (ignored).
/// @param y Top coordinate (ignored).
void rt_videowidget_set_position(void *v, int64_t x, int64_t y) {
    (void)v;
    (void)x;
    (void)y;
}

/// @brief Ignore adding a child to an unavailable VideoWidget.
/// @param v VideoWidget handle (ignored).
/// @param child Child widget handle (ignored).
void rt_videowidget_add_child(void *v, void *child) {
    (void)v;
    (void)child;
}

/* VideoPlayer stubs */

/// @brief Stub for `VideoPlayer.Open` — would normally parse the video
///        file at `p` (.avi via MJPEG decoder, or .ogv via Theora
///        infrastructure) and return a player ready to `Play`.
///
/// Silent stub returning NULL.
///
/// @param p Filesystem path to the video file (ignored).
///
/// @return `NULL`.
void *rt_videoplayer_open(rt_string p) {
    (void)p;
    return NULL;
}

/// @brief Stub for `VideoPlayer.Play` — start or resume playback.
///        From a stopped state, restarts at frame 0; from a paused state,
///        resumes from the current position.
///
/// Silent no-op stub.
///
/// @param v VideoPlayer handle (ignored).
void rt_videoplayer_play(void *v) {
    (void)v;
}

/// @brief Stub for `VideoPlayer.Pause` — pause playback. Frames stop
///        decoding but the audio cursor and `Position` are preserved.
///        Resume with `Play`.
///
/// Silent no-op stub.
///
/// @param v VideoPlayer handle (ignored).
void rt_videoplayer_pause(void *v) {
    (void)v;
}

/// @brief Stub for `VideoPlayer.Stop` — stop playback and rewind to
///        frame 0. Drops any decoded frames in the queue.
///
/// Silent no-op stub.
///
/// @param v VideoPlayer handle (ignored).
void rt_videoplayer_stop(void *v) {
    (void)v;
}

/// @brief Stub for `VideoPlayer.Seek` — jump to time position `s` in
///        seconds. Audio resyncs immediately; video resyncs to the next
///        keyframe (with a brief catch-up decode).
///
/// Silent no-op stub.
///
/// @param v VideoPlayer handle (ignored).
/// @param s Target time in seconds (ignored).
void rt_videoplayer_seek(void *v, double s) {
    (void)v;
    (void)s;
}

/// @brief Stub for `VideoPlayer.Update` — advance video playback by `dt`
///        seconds: decode pending video frames, advance audio cursor,
///        maintain A/V sync.
///
/// Silent no-op stub.
///
/// @param v  VideoPlayer handle (ignored).
/// @param dt Delta time in seconds (ignored).
void rt_videoplayer_update(void *v, double dt) {
    (void)v;
    (void)dt;
}

/// @brief Stub for `VideoPlayer.SetVolume` — set the audio track's
///        playback volume, 0..1.
///
/// Silent no-op stub.
///
/// @param v   VideoPlayer handle (ignored).
/// @param vol Volume, 0..1 (ignored).
void rt_videoplayer_set_volume(void *v, double vol) {
    (void)v;
    (void)vol;
}

/// @brief Stub for `VideoPlayer.Width` — get the video's frame width in
///        pixels (parsed from the stream header at `Open` time).
///
/// Silent stub returning `0`.
///
/// @param v VideoPlayer handle (ignored).
///
/// @return `0`.
int64_t rt_videoplayer_get_width(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub for `VideoPlayer.Height` — get the video's frame height
///        in pixels.
///
/// Silent stub returning `0`.
///
/// @param v VideoPlayer handle (ignored).
///
/// @return `0`.
int64_t rt_videoplayer_get_height(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub for `VideoPlayer.Duration` — get the total length of the
///        video in seconds.
///
/// Silent stub returning `0.0`.
///
/// @param v VideoPlayer handle (ignored).
///
/// @return `0.0`.
double rt_videoplayer_get_duration(void *v) {
    (void)v;
    return 0.0;
}

/// @brief Stub for `VideoPlayer.Position` — get the current playback
///        position in seconds.
///
/// Silent stub returning `0.0`.
///
/// @param v VideoPlayer handle (ignored).
///
/// @return `0.0`.
double rt_videoplayer_get_position(void *v) {
    (void)v;
    return 0.0;
}

/// @brief Stub for `VideoPlayer.IsPlaying` — true while playback is
///        actively advancing (not paused, not stopped, not at EOF).
///
/// Silent stub returning `0`.
///
/// @param v VideoPlayer handle (ignored).
///
/// @return `0`.
int64_t rt_videoplayer_get_is_playing(void *v) {
    (void)v;
    return 0;
}

/// @brief Stub for `VideoPlayer.Frame` — get the most recently decoded
///        video frame as a Pixels surface (RGBA, frame size).
///
/// Silent stub returning NULL.
///
/// @param v VideoPlayer handle (ignored).
///
/// @return `NULL`.
void *rt_videoplayer_get_frame(void *v) {
    (void)v;
    return NULL;
}
