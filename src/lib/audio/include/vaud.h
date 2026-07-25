//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD Public API
//
// Provides a cross-platform audio library for sound effect playback and music
// streaming. The library implements a simple immediate-mode API with a software
// mixer that combines multiple audio sources into a single output stream.
//
// Key design principles:
// - Zero external dependencies (uses only OS-level audio APIs)
// - Software mixing for predictable, portable audio output
// - Thread-safe playback (audio runs on dedicated thread)
// - Simple resource management (load/play/free)
// - WAV file format support (8/16/24/32-bit PCM and 32-bit float)
//
// Supported platforms:
// - macOS (Core Audio / AudioQueue backend)
// - Linux (ALSA backend)
// - Windows (WASAPI backend)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Cross-platform audio library public API.
/// @details Exposes audio context management, sound effect loading and playback,
///          music streaming, and volume control. Playback/control operations are
///          synchronized internally; context/handle destruction must not run
///          concurrently with other operations on the same context or handle.

#pragma once

#include "vaud_config.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Library Version
//===----------------------------------------------------------------------===//

#define VAUD_VERSION_MAJOR 1
#define VAUD_VERSION_MINOR 0
#define VAUD_VERSION_PATCH 0

/// @brief Query the runtime library version as a packed integer.
/// @details Encodes the version as (major << 16) | (minor << 8) | patch.
/// @return Packed version number (e.g., 0x010000 for version 1.0.0).
uint32_t vaud_version(void);

/// @brief Get the library version as a human-readable string.
/// @return Version string (e.g., "1.0.0"), never NULL.
const char *vaud_version_string(void);

//===----------------------------------------------------------------------===//
// Core Data Types
//===----------------------------------------------------------------------===//

/// @brief Opaque handle to an audio context.
/// @details Manages the audio output device, mixer, and all loaded resources.
///          Created via vaud_create() and destroyed via vaud_destroy().
///          A single context is typically sufficient for an application.
typedef struct vaud_context *vaud_context_t;

/// @brief Opaque handle to a loaded sound effect.
/// @details Represents a short audio clip loaded entirely into memory.
///          Optimized for low-latency playback of effects like gunshots,
///          explosions, or UI sounds. Created via vaud_load_sound().
typedef struct vaud_sound *vaud_sound_t;

/// @brief Opaque handle to a music stream.
/// @details Represents a long audio file streamed from disk. Optimized for
///          memory efficiency when playing background music or long audio.
///          Created via vaud_load_music().
typedef struct vaud_music *vaud_music_t;

/// @brief Voice identifier for active playback instances.
/// @details Returned by vaud_play() to allow control of individual playing
///          sounds. Valid until the sound finishes or is stopped.
typedef int32_t vaud_voice_id;

/// @brief Invalid voice ID constant.
#define VAUD_INVALID_VOICE (-1)

/// @brief Query whether a logical mix group has a registered effects processor.
/// @details Called by the software mixer while it holds the ZannaAUD state lock.
///          Implementations must not allocate and should return quickly. Returning
///          zero routes the group directly to the master mix.
/// @param userdata Opaque pointer supplied during processor registration.
/// @param group_id Logical mix group being queried.
/// @return Nonzero when the group should be routed through @c process_fn.
typedef int (*vaud_group_effects_query_fn)(void *userdata, int64_t group_id);

/// @brief Process one logical mix group bus in-place.
/// @details Samples are interleaved floating-point PCM in the conventional
///          `[-1.0, 1.0]` range. Called by the software mixer after summing the
///          group bus and before adding it to the master accumulator.
/// @param userdata Opaque pointer supplied during processor registration.
/// @param group_id Logical mix group represented by @p samples.
/// @param samples Mutable interleaved bus buffer processed in place.
/// @param frames Number of sample frames in the buffer.
/// @param channels Interleaved channel count.
/// @param sample_rate Output sample rate in hertz.
typedef void (*vaud_group_effects_process_fn)(void *userdata,
                                              int64_t group_id,
                                              float *samples,
                                              int32_t frames,
                                              int32_t channels,
                                              int32_t sample_rate);

/// @brief Audio backend and mixer diagnostic counters.
/// @details Counters are monotonic for the lifetime of a context. They are intended for debug
///          panels, smoke probes, and backend triage; applications should not use them for normal
///          playback control. Fields unsupported by a platform backend remain zero.
typedef struct {
    uint64_t render_calls; ///< Mixer render callbacks completed or attempted.
    uint64_t
        mixer_lock_misses; ///< Mixer callbacks that emitted silence due to state-lock contention.
    uint64_t backend_write_calls;    ///< Platform write calls issued to the device API.
    uint64_t backend_partial_writes; ///< Device writes that accepted fewer frames than requested.
    uint64_t backend_waits;          ///< Device waits/polls used to avoid busy retries.
    uint64_t backend_xruns;          ///< Underrun/suspend recoveries observed by the backend.
    uint64_t backend_recoveries;     ///< Successful or attempted backend recovery operations.
    uint64_t backend_write_failures; ///< Device writes that failed after recovery attempts.
} vaud_stats_t;

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

/// @brief Error code enumeration.
typedef enum {
    VAUD_OK = 0,           ///< No error
    VAUD_ERR_ALLOC,        ///< Memory allocation failed
    VAUD_ERR_PLATFORM,     ///< Platform-specific audio error
    VAUD_ERR_FILE,         ///< File I/O error
    VAUD_ERR_FORMAT,       ///< Unsupported audio format
    VAUD_ERR_INVALID_PARAM ///< Invalid parameter
} vaud_error_t;

/// @brief Retrieve the last error message.
/// @return Error message string, or NULL if no error.
const char *vaud_get_last_error(void);

/// @brief Clear the last error state.
void vaud_clear_error(void);

//===----------------------------------------------------------------------===//
// Context Management
//===----------------------------------------------------------------------===//

/// @brief Create a new audio context.
/// @details Initializes the platform audio backend, starts the audio thread,
///          and prepares the software mixer. Only one context is needed per
///          application. Returns NULL on failure.
/// @return Audio context handle on success, NULL on failure.
vaud_context_t vaud_create(void);

/// @brief Destroy an audio context and release device resources.
/// @details Stops all playback, shuts down the audio thread, releases platform
///          resources, and detaches caller-owned sound/music handles so they can
///          still be freed safely after the context is gone. Safe to pass NULL.
///          The caller must ensure no other thread is using @p ctx while this
///          function runs.
/// @param ctx Audio context to destroy (may be NULL).
void vaud_destroy(vaud_context_t ctx);

/// @brief Set the master volume for all audio output.
/// @details Affects all sounds and music proportionally. Does not affect
///          individual sound/music volume settings, which are multiplied
///          with the master volume.
/// @param ctx Audio context.
/// @param volume Master volume (0.0 = silent, 1.0 = full volume). Non-finite
///               values become 0.0; finite out-of-range values are clamped.
void vaud_set_master_volume(vaud_context_t ctx, float volume);

/// @brief Get the current master volume.
/// @param ctx Audio context.
/// @return Current master volume (0.0 to 1.0), or 0.0 if ctx is NULL.
float vaud_get_master_volume(vaud_context_t ctx);

/// @brief Pause all audio playback.
/// @details Suspends the audio thread. All playing sounds and music will
///          pause at their current positions. Use vaud_resume_all() to continue.
/// @param ctx Audio context.
void vaud_pause_all(vaud_context_t ctx);

/// @brief Resume all audio playback.
/// @details Resumes the audio thread after vaud_pause_all(). All sounds and
///          music continue from their paused positions.
/// @param ctx Audio context.
void vaud_resume_all(vaud_context_t ctx);

//===----------------------------------------------------------------------===//
// Sound Effects
//===----------------------------------------------------------------------===//

/// @brief Load a sound effect from a WAV file.
/// @details Reads the entire file into memory and converts to internal format.
///          Supports 8/16/24/32-bit PCM and 32-bit float WAV files, mono or
///          stereo, any sample rate (will be resampled to VAUD_SAMPLE_RATE).
///          WAV headers are validated for block alignment and complete PCM
///          frames; non-critical byte-rate mismatches are tolerated.
/// @param ctx Audio context.
/// @param path Path to the WAV file.
/// @return Sound handle on success, NULL on failure.
vaud_sound_t vaud_load_sound(vaud_context_t ctx, const char *path);

/// @brief Load a sound effect from memory.
/// @details Parses WAV data from a memory buffer. Useful for embedded resources.
///          WAV headers are validated for block alignment and complete PCM
///          frames; non-critical byte-rate mismatches are tolerated.
/// @param ctx Audio context.
/// @param data Pointer to WAV file data.
/// @param size Size of the data in bytes.
/// @return Sound handle on success, NULL on failure.
vaud_sound_t vaud_load_sound_mem(vaud_context_t ctx, const void *data, size_t size);

/// @brief Free a loaded sound effect.
/// @details Releases the memory used by the sound. Any voices currently playing
///          this sound will be stopped. Safe to pass NULL. The caller must
///          ensure no other thread is using @p sound while this function runs.
/// @param sound Sound to free (may be NULL).
void vaud_free_sound(vaud_sound_t sound);

/// @brief Detach a sound from its owning context without freeing its sample data.
/// @details Used during runtime shutdown so higher-level wrappers can survive
///          `vaud_destroy()` and free themselves later without touching a dead context.
/// @param sound Sound to detach (may be NULL).
void vaud_detach_sound(vaud_sound_t sound);

/// @brief Return non-zero when a sound is still attached to a live context.
/// @param sound Sound to inspect (may be NULL).
/// @return 1 when attached to a context that is not being destroyed; otherwise 0.
int vaud_sound_is_attached(vaud_sound_t sound);

/// @brief Play a sound effect.
/// @details Starts playback immediately using the next available voice. If all
///          voices are in use, the oldest non-looping voice is stolen.
/// @param sound Sound to play.
/// @return Voice ID for controlling playback, or VAUD_INVALID_VOICE on failure.
vaud_voice_id vaud_play(vaud_sound_t sound);

/// @brief Play a sound effect with volume and pan control.
/// @details Extended version of vaud_play() with per-instance settings.
/// @param sound Sound to play.
/// @param volume Playback volume (0.0 to 1.0). Non-finite values become 0.0;
///               finite out-of-range values are clamped.
/// @param pan Stereo pan (-1.0 = left, 0.0 = center, 1.0 = right). Non-finite
///            values become center; finite out-of-range values are clamped.
/// @return Voice ID for controlling playback, or VAUD_INVALID_VOICE on failure.
vaud_voice_id vaud_play_ex(vaud_sound_t sound, float volume, float pan);

/// @brief Play a sound effect with volume, pan, and logical mix-group routing.
/// @param sound Sound to play.
/// @param volume Playback volume (0.0 to 1.0).
/// @param pan Stereo pan (-1.0 to 1.0).
/// @param group_id Logical group id used by an optional effects processor.
/// @return Voice ID for controlling playback, or VAUD_INVALID_VOICE on failure.
vaud_voice_id vaud_play_ex_group(vaud_sound_t sound, float volume, float pan, int64_t group_id);

/// @brief Play a sound effect with looping.
/// @details Starts looped playback that continues until explicitly stopped.
/// @param sound Sound to play.
/// @param volume Playback volume (0.0 to 1.0). Non-finite values become 0.0;
///               finite out-of-range values are clamped.
/// @param pan Stereo pan (-1.0 = left, 0.0 = center, 1.0 = right). Non-finite
///            values become center; finite out-of-range values are clamped.
/// @return Voice ID for controlling playback, or VAUD_INVALID_VOICE on failure.
vaud_voice_id vaud_play_loop(vaud_sound_t sound, float volume, float pan);

/// @brief Play a looping sound effect with logical mix-group routing.
/// @param sound Sound to play.
/// @param volume Playback volume, normalized like vaud_play_ex().
/// @param pan Stereo pan, normalized like vaud_play_ex().
/// @param group_id Logical group routed through optional group processing.
/// @return Voice ID for controlling playback, or VAUD_INVALID_VOICE on failure.
vaud_voice_id vaud_play_loop_group(vaud_sound_t sound, float volume, float pan, int64_t group_id);

/// @brief Stop a playing voice.
/// @details Immediately stops playback of the specified voice.
/// @param ctx Audio context.
/// @param voice Voice ID returned by vaud_play().
void vaud_stop_voice(vaud_context_t ctx, vaud_voice_id voice);

/// @brief Set the volume of a playing voice.
/// @param ctx Audio context.
/// @param voice Voice ID.
/// @param volume New volume (0.0 to 1.0). Non-finite values become 0.0;
///               finite out-of-range values are clamped.
void vaud_set_voice_volume(vaud_context_t ctx, vaud_voice_id voice, float volume);

/// @brief Set the pan of a playing voice.
/// @param ctx Audio context.
/// @param voice Voice ID.
/// @param pan New pan (-1.0 = left, 0.0 = center, 1.0 = right). Non-finite
///            values become center; finite out-of-range values are clamped.
void vaud_set_voice_pan(vaud_context_t ctx, vaud_voice_id voice, float pan);

/// @brief Move a currently playing voice to a logical mix group.
/// @param ctx Audio context.
/// @param voice Voice ID to reroute.
/// @param group_id New logical mix-group identifier.
void vaud_set_voice_group(vaud_context_t ctx, vaud_voice_id voice, int64_t group_id);

/// @brief Check if a voice is still playing.
/// @param ctx Audio context.
/// @param voice Voice ID.
/// @return 1 if playing, 0 if stopped or invalid.
int vaud_voice_is_playing(vaud_context_t ctx, vaud_voice_id voice);

/// @brief Play a sound with volume, pan, and pitch (playback-rate) control.
/// @param sound Sound to play.
/// @param volume Playback volume (0.0 to 1.0).
/// @param pan Stereo pan (-1.0 to 1.0).
/// @param pitch Playback-rate multiplier (clamped to 0.25–4.0; 1.0 = native).
/// @return Voice ID for controlling playback, or VAUD_INVALID_VOICE on failure.
vaud_voice_id vaud_play_ex2(vaud_sound_t sound, float volume, float pan, float pitch);

/// @brief Set the playback-rate (pitch) multiplier of a playing voice.
/// @details Values are clamped to 0.25–4.0; non-positive/NaN values reset to
///          1.0. Rates other than 1.0 resample with linear interpolation
///          (sub-sample fractional cursor), so pitch and duration both scale.
/// @param ctx Audio context.
/// @param voice Voice ID.
/// @param pitch Playback-rate multiplier (1.0 = native rate).
void vaud_set_voice_pitch(vaud_context_t ctx, vaud_voice_id voice, float pitch);

/// @brief Get the playback-rate (pitch) multiplier of a voice (1.0 default).
/// @param ctx Audio context.
/// @param voice Voice ID to query.
/// @return Current multiplier, or 1.0 for an invalid/unavailable voice.
float vaud_get_voice_pitch(vaud_context_t ctx, vaud_voice_id voice);

/// @brief Set a direct per-voice one-pole lowpass cutoff in Hz.
/// @details Values <= 0 (or NaN) bypass the filter. Composes with occlusion:
///          the effective cutoff is the lower of the two.
/// @param ctx Audio context.
/// @param voice Voice ID to update.
/// @param cutoff_hz Requested cutoff, or a nonpositive value to bypass.
void vaud_set_voice_lowpass(vaud_context_t ctx, vaud_voice_id voice, float cutoff_hz);

/// @brief Set the occlusion amount of a voice (0 = open .. 1 = fully occluded).
/// @details Maps to a perceptual lowpass sweep (~22 kHz down to ~800 Hz) plus
///          up to -6 dB of gain reduction. Changes are smoothed inside the
///          mixer (~80 ms) so gameplay-driven toggles never zipper.
/// @param ctx Audio context.
/// @param voice Voice ID to update.
/// @param amount Occlusion amount; finite values are clamped to [0, 1], while
///               nonpositive and NaN values become zero.
void vaud_set_voice_occlusion(vaud_context_t ctx, vaud_voice_id voice, float amount);

/// @brief Enable/disable per-voice RMS metering (zero mixing cost when off).
/// @param ctx Audio context.
/// @param voice Voice ID to update.
/// @param enabled Nonzero to enable metering; zero to disable and clear level.
void vaud_set_voice_metering(vaud_context_t ctx, vaud_voice_id voice, int enabled);

/// @brief RMS source level (pre-gain) of the last mixed block; 0 when unmetered.
/// @param ctx Audio context.
/// @param voice Voice ID to query.
/// @return Last metered source RMS, or 0 for invalid, stopped, or unmetered voices.
float vaud_get_voice_level(vaud_context_t ctx, vaud_voice_id voice);

/// @brief Register, replace, or remove a sidechain-style group ducking rule.
/// @details While any voice in @p trigger_group is audible, the gain of
///          @p target_group eases toward (1 - amount) over @p attack_sec and
///          recovers to unity over @p release_sec. Re-registering the same
///          (trigger, target) pair replaces the rule; amount <= 0 removes it.
///          At most VAUD-internal rule-table capacity (8) rules are active.
/// @param ctx Audio context.
/// @param trigger_group Group whose audible voices activate ducking.
/// @param target_group Group whose gain is reduced.
/// @param amount Maximum gain reduction, clamped to [0, 1]; nonpositive removes.
/// @param attack_sec Positive fade-down time; invalid values become 1 ms.
/// @param release_sec Positive recovery time; invalid values become 1 ms.
void vaud_set_group_duck(vaud_context_t ctx,
                         int64_t trigger_group,
                         int64_t target_group,
                         float amount,
                         float attack_sec,
                         float release_sec);

//===----------------------------------------------------------------------===//
// Music Streaming
//===----------------------------------------------------------------------===//

/// @brief Load music from a WAV file for streaming playback.
/// @details Opens the file for streaming. Only a small buffer is loaded into
///          memory at a time, making this suitable for long audio files.
///          Calling vaud_music_play() on a stopped stream restarts from the
///          beginning, including after end-of-file.
/// @param ctx Audio context.
/// @param path Path to the WAV file.
/// @return Music handle on success, NULL on failure.
vaud_music_t vaud_load_music(vaud_context_t ctx, const char *path);

/// @brief Load a Vorbis logical stream from an OGG container for streaming music playback.
/// @details Accepts plain `.ogg` audio files and mixed logical-stream containers
///          such as `.ogv`, selecting the first Vorbis stream it finds. Mono or
///          stereo input is resampled to the configured output sample rate.
/// @param ctx Audio context that owns and services the stream.
/// @param path Path to the OGG container.
/// @return Music handle on success, or NULL on open, decode, format, or allocation failure.
vaud_music_t vaud_load_music_ogg(vaud_context_t ctx, const char *path);

/// @brief Load an MP3 file for streaming music playback.
/// @details Opens decoder state and buffers decoded mono or stereo frames for
///          resampling to the configured output rate.
/// @param ctx Audio context that owns and services the stream.
/// @param path Path to the MP3 file.
/// @return Music handle on success, or NULL on open, format, or allocation failure.
vaud_music_t vaud_load_music_mp3(vaud_context_t ctx, const char *path);

/// @brief Service streaming music buffers outside the audio render callback.
/// @details Decodes/refills empty music buffers and processes pending loop rewinds.
///          Applications using the high-level Zanna runtime should call
///          `Zanna.Sound.Audio.Update()` each frame; it forwards here. The
///          realtime mixer consumes decoded buffers and never performs file I/O
///          or codec decode work.
/// @param ctx Audio context.
void vaud_update(vaud_context_t ctx);

/// @brief Free a loaded music stream.
/// @details Stops playback if playing, closes the file, and frees resources.
///          The caller must ensure no other thread is using @p music while this
///          function runs.
/// @param music Music to free (may be NULL).
void vaud_free_music(vaud_music_t music);

/// @brief Detach a music stream from its owning context without freeing decoder state.
/// @details Used during runtime shutdown so wrapper-owned finalizers can release
///          the music later without touching a destroyed context.
/// @param music Music to detach (may be NULL).
void vaud_detach_music(vaud_music_t music);

/// @brief Return non-zero when a music stream is still attached to a live context.
/// @param music Music stream to inspect (may be NULL).
/// @return 1 when attached to a context that is not being destroyed; otherwise 0.
int vaud_music_is_attached(vaud_music_t music);

/// @brief Start music playback.
/// @details Begins streaming playback from the beginning of the file.
/// @param music Music to play.
/// @param loop Non-zero for looped playback, zero for one-shot.
void vaud_music_play(vaud_music_t music, int loop);

/// @brief Stop music playback.
/// @details Stops playback and resets position to the beginning.
/// @param music Music to stop.
void vaud_music_stop(vaud_music_t music);

/// @brief Pause music playback.
/// @details Pauses at the current position. Use vaud_music_resume() to continue.
/// @param music Music to pause.
void vaud_music_pause(vaud_music_t music);

/// @brief Resume paused music playback.
/// @param music Music to resume.
void vaud_music_resume(vaud_music_t music);

/// @brief Update the loop flag for a music stream without changing its play state.
/// @param music Music handle.
/// @param loop Non-zero for looped playback, zero for one-shot.
void vaud_music_set_loop(vaud_music_t music, int loop);

/// @brief Set music playback volume.
/// @param music Music handle.
/// @param volume Volume (0.0 to 1.0). Non-finite values become 0.0; finite
///               out-of-range values are clamped.
void vaud_music_set_volume(vaud_music_t music, float volume);

/// @brief Assign a music stream to a logical mix group.
/// @param music Music handle; NULL is ignored.
/// @param group_id New logical mix-group identifier.
void vaud_music_set_group(vaud_music_t music, int64_t group_id);

/// @brief Get music playback volume.
/// @param music Music handle.
/// @return Current volume, or 0.0 if music is NULL.
float vaud_music_get_volume(vaud_music_t music);

/// @brief Check if music is currently playing.
/// @param music Music handle.
/// @return 1 if playing, 0 if stopped/paused or NULL.
int vaud_music_is_playing(vaud_music_t music);

/// @brief Seek to a position in the music.
/// @details Seeks to the specified time offset. Non-finite values are ignored;
///          finite values are clamped to the stream duration when known. May
///          cause a brief audio gap.
/// @param music Music handle.
/// @param seconds Time offset in seconds from the beginning.
void vaud_music_seek(vaud_music_t music, float seconds);

/// @brief Get the current playback position.
/// @param music Music handle.
/// @return Current position in seconds, or 0.0 if NULL.
float vaud_music_get_position(vaud_music_t music);

/// @brief Get the total duration of the music.
/// @param music Music handle.
/// @return Duration in seconds, or 0.0 if NULL.
float vaud_music_get_duration(vaud_music_t music);

//===----------------------------------------------------------------------===//
// Utility Functions
//===----------------------------------------------------------------------===//

/// @brief Get the number of active voices.
/// @param ctx Audio context.
/// @return Number of voices currently playing.
int32_t vaud_get_active_voice_count(vaud_context_t ctx);

/// @brief Stop all playing sounds (but not music).
/// @param ctx Audio context.
void vaud_stop_all_sounds(vaud_context_t ctx);

/// @brief Get the audio latency in milliseconds.
/// @details Returns the approximate time between a play call and audible output.
/// @param ctx Audio context.
/// @return Latency in milliseconds.
float vaud_get_latency_ms(vaud_context_t ctx);

/// @brief Copy audio mixer/backend diagnostic counters for @p ctx.
/// @details The snapshot is lock-free and may race slightly with the audio thread, but each field
///          is read atomically. Passing NULL for either argument is a no-op.
/// @param ctx Audio context to inspect.
/// @param out_stats Destination statistics snapshot.
void vaud_get_stats(vaud_context_t ctx, vaud_stats_t *out_stats);

/// @brief Install or clear the optional logical mix-group effects processor.
/// @details Passing NULL for @p process_fn disables group processing. The
///          callbacks are invoked only from the software mixer while the audio
///          context mutex is held, so they must not call back into ZannaAUD,
///          perform allocation, blocking I/O, or context destruction.
/// @param ctx Audio context whose group processor is replaced.
/// @param query_fn Optional fast predicate selecting processed groups.
/// @param process_fn In-place processor, or NULL to clear all callback state.
/// @param userdata Opaque callback state retained only while @p process_fn is set.
void vaud_set_group_effects_processor(vaud_context_t ctx,
                                      vaud_group_effects_query_fn query_fn,
                                      vaud_group_effects_process_fn process_fn,
                                      void *userdata);

#ifdef __cplusplus
}
#endif
