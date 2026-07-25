//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/audio/rt_audio.h
// Purpose: Runtime bridge for the ZannaAUD audio library, exposing sound effect and music playback
// controls (play, pause, stop, volume, loop) over opaque audio handles.
//
// Key invariants:
//   - All sound and music pointers are opaque handles returned by rt_sound_new/rt_music_new.
//   - Volume is in the range [0, 100]; values are clamped to this range.
//   - Looping and channel multiplexing are managed by the underlying ZannaAUD backend.
//
// Ownership/Lifetime:
//   - Sounds must be freed via rt_sound_destroy; music via rt_music_destroy.
//   - Caller owns all handles returned by factory functions.
//
// Links: src/lib/audio/include/vaud.h (underlying audio backend), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the C runtime ABI for audio system control, sound voices,
///        streamed music, crossfade coordination, and audio diagnostics.
/// @details Sound and music objects are opaque reference-counted handles.
///          Loading returns a caller-owned handle that must be released with
///          the matching destroy function. Playback returns transient voice
///          identifiers rather than owning handles. Public percentages and pan
///          values are normalized by the implementation, and callers can use
///          @ref rt_audio_is_available before invoking backend-dependent
///          operations in builds compiled without audio support.

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// Audio System Management
//=========================================================================

/// @brief Report whether audio support is compiled into this runtime.
/// @details This is a compile-time capability query and does not initialize the
///          device or guarantee that later device initialization will succeed.
/// @return `1` when the audio backend is compiled in, otherwise `0`.
int8_t rt_audio_is_available(void);

/// @brief Initialize the audio system.
/// @details Creates the process-wide backend context. Backend-dependent load
///          operations also initialize it lazily, so explicit initialization
///          is useful when callers need to handle device failure up front.
/// @return `1` on success or when already initialized; `0` on failure.
int64_t rt_audio_init(void);

/// @brief Shutdown the audio system.
/// @details Stops playback, cancels crossfades, releases backend resources, and
///          permits later reinitialization. Caller-owned sound/music wrappers
///          remain valid runtime objects but are detached and cannot play; they
///          must still be released with their destroy functions.
void rt_audio_shutdown(void);

/// @brief Set the master volume for all audio output.
/// @details The logical value is retained even before initialization or when no
///          audio backend is compiled in.
/// @param volume Requested master volume, clamped to `[0, 100]`.
void rt_audio_set_master_volume(int64_t volume);

/// @brief Get the current master volume.
/// @return Current logical master volume in `[0, 100]`.
int64_t rt_audio_get_master_volume(void);

/// @brief Pause all audio playback.
/// @details Pauses active voices and streams and freezes automatic crossfade
///          advancement until @ref rt_audio_resume_all is called.
void rt_audio_pause_all(void);

/// @brief Resume all audio playback.
/// @details Resumes playback paused by @ref rt_audio_pause_all. Per-stream
///          pause state remains controlled by the music pause APIs.
void rt_audio_resume_all(void);

/// @brief Advance time-based audio state such as music crossfades.
/// @details Call once per frame when using crossfades outside Playlist.Update().
///          The runtime uses its monotonic clock to derive elapsed time.
void rt_audio_update(void);

/// @brief Stop all playing sounds (but not music).
/// @details Invalidates the runtime's tracking for all transient sound voices;
///          streamed music and crossfade state are unaffected.
void rt_audio_stop_all_sounds(void);

/// @brief Mixer render callbacks attempted by the active audio context.
/// @return Saturating monotonic count, or `0` when no context is active.
int64_t rt_audio_get_render_calls(void);
/// @brief Mixer callbacks that emitted silence because the state lock was busy.
/// @return Saturating monotonic count, or `0` when no context is active.
int64_t rt_audio_get_mixer_lock_misses(void);
/// @brief Platform backend write calls issued by the active audio context.
/// @return Saturating monotonic count, or `0` when unsupported/inactive.
int64_t rt_audio_get_backend_write_calls(void);
/// @brief Platform backend writes that accepted fewer frames than requested.
/// @return Saturating monotonic partial-write count, or `0` when unsupported.
int64_t rt_audio_get_backend_partial_writes(void);
/// @brief Platform backend waits used to avoid busy retry loops.
/// @return Saturating monotonic wait count, or `0` when unsupported.
int64_t rt_audio_get_backend_waits(void);
/// @brief Platform backend underrun or suspend recoveries observed.
/// @return Saturating monotonic xrun/suspend count, or `0` when unsupported.
int64_t rt_audio_get_backend_xruns(void);
/// @brief Platform backend recovery attempts.
/// @return Saturating monotonic recovery-attempt count, or `0` when unsupported.
int64_t rt_audio_get_backend_recoveries(void);
/// @brief Platform backend writes that failed after recovery attempts.
/// @return Saturating monotonic unrecovered-failure count, or `0` when unsupported.
int64_t rt_audio_get_backend_write_failures(void);

//=========================================================================
// Sound Effects
//=========================================================================

/// @brief Load a sound effect from a WAV, OGG, or MP3 file.
/// @details OGG and MP3 input is decoded before backend loading. The returned
///          wrapper may produce multiple concurrent voices.
/// @param path Runtime string containing the audio file path.
/// @return Caller-owned opaque sound handle, or NULL on failure.
void *rt_sound_load(rt_string path);

/// @brief Load a sound effect through the runtime asset manager.
/// @details The asset bytes are temporary and are not retained after loading.
/// @param name Mounted/embedded asset name, asset:// URI, or dev filesystem path.
/// @return Caller-owned opaque sound handle, or NULL on failure.
void *rt_sound_load_asset(rt_string name);

/// @brief Load a sound effect from in-memory WAV, OGG, or MP3 data.
/// @details The input bytes are borrowed for the duration of the call and are
///          neither modified nor retained.
/// @param data Pointer to readable encoded audio data.
/// @param size Size of @p data in bytes; must be positive.
/// @return Caller-owned opaque sound handle, or NULL on failure.
void *rt_sound_load_mem(const void *data, int64_t size);

/// @brief Release a loaded sound effect handle.
/// @details NULL and invalid handles are ignored. The final reference releases
///          the backend sound resource.
/// @param sound Sound handle returned by any sound load operation.
void rt_sound_destroy(void *sound);

/// @brief Play a sound effect with default settings.
/// @details Uses full logical volume, centered pan, no looping, and the built-in
///          sound-effects mix group.
/// @param sound Live sound handle.
/// @return Voice ID for controlling playback, or `-1` on failure.
int64_t rt_sound_play(void *sound);

/// @brief Play a sound effect with volume and pan control.
/// @param sound Live sound handle.
/// @param volume Logical playback volume, clamped to `[0, 100]`.
/// @param pan Stereo pan, clamped to `[-100, 100]`.
/// @return Voice ID for controlling playback, or `-1` on failure.
int64_t rt_sound_play_ex(void *sound, int64_t volume, int64_t pan);

/// @brief Play a sound effect with looping.
/// @param sound Live sound handle.
/// @param volume Logical playback volume, clamped to `[0, 100]`.
/// @param pan Stereo pan, clamped to `[-100, 100]`.
/// @return Voice ID for controlling playback, or `-1` on failure.
int64_t rt_sound_play_loop(void *sound, int64_t volume, int64_t pan);

/// @brief Stop a playing voice.
/// @details Negative or stale identifiers are ignored.
/// @param voice_id Voice ID returned by a sound playback function.
void rt_voice_stop(int64_t voice_id);

/// @brief Set the volume of a playing voice.
/// @details The value is stored as the voice's logical pre-group volume, then
///          recombined with its current mix-group multiplier.
/// @param voice_id Voice ID returned by a sound playback function.
/// @param volume New logical volume, clamped to `[0, 100]`.
void rt_voice_set_volume(int64_t voice_id, int64_t volume);

/// @brief Set the pan of a playing voice.
/// @param voice_id Voice ID returned by a sound playback function.
/// @param pan New pan clamped to `[-100, 100]`.
void rt_voice_set_pan(int64_t voice_id, int64_t pan);

/// @brief Check if a voice is still playing.
/// @param voice_id Voice ID returned by a sound playback function.
/// @return `1` if playing; `0` if stopped, invalid, or unavailable.
int64_t rt_voice_is_playing(int64_t voice_id);

/// @brief Play a sound with volume (0-100), pan (-100..100), and pitch.
/// @param sound Live sound handle.
/// @param volume Logical playback volume, clamped to `[0, 100]`.
/// @param pan Stereo pan, clamped to `[-100, 100]`.
/// @param pitch Playback-rate multiplier (clamped 0.25–4.0; 1.0 = native).
/// @return Voice ID for controlling playback, or `-1` on failure.
int64_t rt_sound_play_ex2(void *sound, int64_t volume, int64_t pan, double pitch);

/// @brief Set a voice's playback-rate (pitch) multiplier (0.25–4.0).
/// @param voice_id Voice ID returned by a sound playback function.
/// @param pitch Requested rate multiplier; `1.0` preserves the source rate.
void rt_voice_set_pitch(int64_t voice_id, double pitch);

/// @brief Get a voice's playback-rate (pitch) multiplier (1.0 default).
/// @param voice_id Voice ID returned by a sound playback function.
/// @return Current multiplier, or `1.0` when unavailable or invalid.
double rt_voice_get_pitch(int64_t voice_id);

/// @brief Set a direct per-voice lowpass cutoff in Hz (<= 0 disables).
/// @details The lower of this cutoff and the occlusion-derived cutoff wins.
/// @param voice_id Voice ID returned by a sound playback function.
/// @param cutoff_hz Cutoff frequency; non-positive disables the direct filter.
void rt_voice_set_lowpass(int64_t voice_id, double cutoff_hz);

/// @brief Set a voice's occlusion amount (0 open .. 1 occluded, smoothed).
/// @param voice_id Voice ID returned by a sound playback function.
/// @param amount Requested occlusion fraction.
void rt_voice_set_occlusion(int64_t voice_id, double amount);
/// @brief Enable/disable per-voice RMS metering (lip-sync level tap).
/// @param voice_id Voice ID returned by a sound playback function.
/// @param enabled Non-zero to enable metering; zero to disable it.
void rt_voice_enable_metering(int64_t voice_id, int8_t enabled);
/// @brief RMS source level (pre-gain) of the last mixed block; 0 when unmetered.
/// @param voice_id Voice ID returned by a sound playback function.
/// @return Most recent RMS level, or `0` when unavailable.
double rt_voice_get_level(int64_t voice_id);

/// @brief Register/replace/remove a sidechain ducking rule between mix groups.
/// @details amount <= 0 removes the (trigger, target) rule.
/// @param trigger_group Group whose audible activity activates the rule.
/// @param target_group Group whose output gain is reduced.
/// @param amount Requested attenuation fraction.
/// @param attack_sec Seconds over which attenuation is applied.
/// @param release_sec Seconds over which full gain is restored.
void rt_audio_set_group_ducking(rt_string trigger_group,
                                rt_string target_group,
                                double amount,
                                double attack_sec,
                                double release_sec);

/// @brief Return non-zero when @p sound is a live runtime Sound wrapper.
/// @details This is intentionally a runtime-handle check, not a playback-capability
///          check. A detached Sound after Audio.Shutdown() is still a Sound object,
///          but playback will fail until a new handle is loaded.
/// @param sound Opaque pointer to examine.
/// @return Non-zero for a registered runtime Sound wrapper.
int64_t rt_sound_is_handle(void *sound);

/// @brief True when the wrapper's backend Sound is attached to the live audio
///        context (stricter than rt_sound_is_handle; false after Shutdown).
/// @param sound Opaque pointer to examine.
/// @return Non-zero only when @p sound can currently be submitted for playback.
int64_t rt_sound_is_playable(void *sound);

//=========================================================================
// Music Streaming
//=========================================================================

/// @brief Load music from a WAV, OGG, or MP3 file for playback.
/// @details Music uses a streaming backend object rather than a reusable
///          transient sound buffer.
/// @param path Runtime string containing the audio file path.
/// @return Caller-owned opaque music handle, or NULL on failure.
void *rt_music_load(rt_string path);

/// @brief Release a loaded music stream handle.
/// @details NULL and invalid handles are ignored. The final reference releases
///          the backend stream.
/// @param music Music handle from rt_music_load.
void rt_music_destroy(void *music);

/// @brief Start music playback.
/// @details Makes this track the foreground stream, resolving unrelated
///          playback/crossfade ownership before starting it.
/// @param music Live music handle.
/// @param loop Non-zero for looped playback, zero for one-shot.
void rt_music_play(void *music, int64_t loop);

/// @brief Stop music playback.
/// @details Resets the stream position and consistently cancels a related
///          crossfade slot when present.
/// @param music Live music handle.
void rt_music_stop(void *music);

/// @brief Pause music playback.
/// @details Pauses both sides when the track belongs to an active crossfade.
/// @param music Live music handle.
void rt_music_pause(void *music);

/// @brief Resume paused music playback.
/// @details Resumes both sides and rebases timing when the track belongs to a
///          paused crossfade.
/// @param music Live music handle.
void rt_music_resume(void *music);

/// @brief Update the loop flag for a music stream without changing play state.
/// @param music Music handle.
/// @param loop Non-zero for looped playback, zero for one-shot.
void rt_music_set_loop(void *music, int64_t loop);

/// @brief Set music playback volume.
/// @details Updates the appropriate envelope anchor when the stream is taking
///          part in an active crossfade.
/// @param music Live music handle.
/// @param volume Logical pre-group volume, clamped to `[0, 100]`.
void rt_music_set_volume(void *music, int64_t volume);

/// @brief Get music playback volume.
/// @param music Live music handle.
/// @return Current logical volume in `[0, 100]`, or `0` if invalid.
int64_t rt_music_get_volume(void *music);

/// @brief Check if music is currently playing.
/// @param music Live music handle.
/// @return `1` if playing; `0` if stopped, paused, invalid, or unavailable.
int64_t rt_music_is_playing(void *music);

/// @brief Seek to a position in the music.
/// @param music Music handle.
/// @param position_ms Time offset in milliseconds from the beginning.
/// @details Repositions only this stream; does not stop unrelated music.
void rt_music_seek(void *music, int64_t position_ms);

/// @brief Get the current playback position.
/// @param music Live music handle.
/// @return Rounded position in milliseconds, or `0` if invalid/unavailable.
int64_t rt_music_get_position(void *music);

/// @brief Get the total duration of the music.
/// @param music Live music handle.
/// @return Rounded duration in milliseconds, or `0` if invalid/unavailable.
int64_t rt_music_get_duration(void *music);

/// @brief Return non-zero when @p music is a live runtime Music wrapper.
/// @param music Opaque pointer to examine.
/// @return Non-zero for a registered runtime Music wrapper.
int64_t rt_music_is_handle(void *music);

/// @brief Pause a music stream and any active crossfade companion tied to it.
/// @details Pauses the companion streams and freezes the shared crossfade clock.
/// @param music Music handle.
void rt_music_pause_related(void *music);

/// @brief Resume a music stream and any active crossfade companion tied to it.
/// @details Restores playback and foreground ownership for the related stream(s).
/// @param music Music handle.
void rt_music_resume_related(void *music);

/// @brief Stop a music stream and any active crossfade companion tied to it.
/// @details Cancels the shared transition while restoring the surviving side's
///          logical volume when applicable.
/// @param music Music handle.
void rt_music_stop_related(void *music);

/// @brief Apply one logical volume to both sides of an active crossfade containing @p music.
/// @details Outside a crossfade this behaves like @ref rt_music_set_volume for
///          the single stream.
/// @param music Music handle.
/// @param volume Logical volume clamped to `[0, 100]`.
void rt_music_set_crossfade_pair_volume(void *music, int64_t volume);

#ifdef __cplusplus
}
#endif
