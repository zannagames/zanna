//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/media/rt_videoplayer.h
// Purpose: Video playback runtime class — loads AVI (MJPEG) and OGG/Theora
//   containers, decodes video frames to Pixels, and presents them for software
//   rendering.
//
// Key invariants:
//   - AVI playback keeps a stable display Pixels plus a reusable decode scratch Pixels.
//   - AVI sync: frame-rate-based (no timestamps in AVI).
//   - Ogg granule positions establish frame count/duration; playback advances by fps or the
//     runtime audio clock when an audio track is active.
//   - Update(dt) must be called each game frame to advance decode.
//   - Open succeeds only after allocating and decoding the initial display frame.
//
// Ownership/Lifetime:
//   - VideoPlayer is runtime-managed and owns file bytes, decoder/index state, and frame buffers.
//   - The Frame property is borrowed and remains owned by the player.
//
// Links: src/runtime/graphics/media/rt_avi.h, src/runtime/graphics/media/rt_theora.h,
//        src/runtime/graphics/2d/rt_pixels.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Declares the runtime-managed AVI/MJPEG and Ogg/Theora video player.
 * @details Exposes player lifecycle controls, time-based frame advancement,
 * seeking, volume, stream metadata, and borrowed access to the stable display
 * Pixels object. File, decoder, audio, and frame-buffer ownership remains with
 * the player.
 */
#pragma once

#include "rt_string.h"
#include <stdint.h>

/** Runtime class identifier stored on managed VideoPlayer payloads. */
#define RT_VIDEOPLAYER_CLASS_ID INT64_C(-0x520120)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Open a video file (AVI/MJPEG or OGG/Theora) and return a player handle (NULL on failure).
/// @details Loads the complete file, validates a supported container/codec, initializes indexes or
///          Theora state, and decodes the first frame before returning.
/// @param path Runtime UTF-8 filesystem path.
/// @return New runtime-managed player, or NULL for I/O, format, codec, decode, or allocation
///         failure.
void *rt_videoplayer_open(rt_string path);

/// @brief Begin or resume playback (decoding advances on each Update).
/// @param vp VideoPlayer handle; invalid handles are ignored.
void rt_videoplayer_play(void *vp);

/// @brief Pause playback (decoding halts; current frame remains displayed).
/// @param vp VideoPlayer handle; invalid handles are ignored.
void rt_videoplayer_pause(void *vp);

/// @brief Stop playback and rewind to the start.
/// @param vp VideoPlayer handle; invalid handles are ignored.
void rt_videoplayer_stop(void *vp);

/// @brief Seek to @p seconds into the stream (clamped to [0, duration]).
/// @details On decode failure, preserves the prior displayed frame and playback position.
/// @param vp VideoPlayer handle; invalid handles are ignored.
/// @param seconds Target timestamp; NaN and negative values map to zero.
void rt_videoplayer_seek(void *vp, double seconds);

/// @brief Advance decoding by @p dt seconds (call once per game frame).
/// @param vp VideoPlayer handle; invalid handles are ignored.
/// @param dt Positive finite elapsed time in seconds.
void rt_videoplayer_update(void *vp, double dt);

/// @brief Set the audio track volume (0.0 = mute, 1.0 = full).
/// @param vp VideoPlayer handle; invalid handles are ignored.
/// @param vol Requested volume; finite values are clamped to [0, 1], while NaN is retained.
void rt_videoplayer_set_volume(void *vp, double vol);

/// @brief Get the video frame width in pixels.
/// @param vp VideoPlayer handle.
/// @return Positive width, or 0 for an invalid handle.
int64_t rt_videoplayer_get_width(void *vp);

/// @brief Get the video frame height in pixels.
/// @param vp VideoPlayer handle.
/// @return Positive height, or 0 for an invalid handle.
int64_t rt_videoplayer_get_height(void *vp);

/// @brief Get the total stream duration in seconds.
/// @param vp VideoPlayer handle.
/// @return Non-negative duration, or 0.0 for an invalid handle.
double rt_videoplayer_get_duration(void *vp);

/// @brief Get the current playback position in seconds.
/// @param vp VideoPlayer handle.
/// @return Active audio-clock position when available, otherwise video-clock position; 0.0 for an
///         invalid handle.
double rt_videoplayer_get_position(void *vp);

/// @brief 1 if currently playing, 0 if paused/stopped/at-end.
/// @param vp VideoPlayer handle.
/// @return 1 while playing, otherwise 0.
int64_t rt_videoplayer_get_is_playing(void *vp);

/// @brief Borrow the player's stable current display Pixels.
/// @param vp VideoPlayer handle.
/// @return Borrowed stable display Pixels, or NULL for an invalid handle.
void *rt_videoplayer_get_frame(void *vp);

#ifdef __cplusplus
}
#endif
