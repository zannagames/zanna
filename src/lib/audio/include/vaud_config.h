//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD Configuration
//
// Build-time configuration constants for the ZannaAUD audio library.
// These values can be overridden via compiler defines if needed.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief ZannaAUD build configuration constants.
/// @details Defines overridable output format, buffering, resource limits, and
///          defaults, plus the single platform-backend selection macro derived
///          from the compiler target.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Audio Format Configuration
//===----------------------------------------------------------------------===//

/// @brief Internal sample rate in Hz.
/// @details All audio is converted to this rate for mixing. 44100 Hz is the
///          CD-quality standard and provides good quality with reasonable CPU usage.
#ifndef VAUD_SAMPLE_RATE
#define VAUD_SAMPLE_RATE 44100
#endif

/// @brief Number of audio channels (1 = mono, 2 = stereo).
/// @details Stereo output is standard for game audio with panning support.
#ifndef VAUD_CHANNELS
#define VAUD_CHANNELS 2
#endif

/// @brief Number of frames per audio buffer.
/// @details Determines latency: frames / sample_rate = seconds.
///          1024 frames at 44100 Hz = ~23ms latency.
///          Smaller values reduce latency but increase CPU overhead.
#ifndef VAUD_BUFFER_FRAMES
#define VAUD_BUFFER_FRAMES 1024
#endif

//===----------------------------------------------------------------------===//
// Resource Limits
//===----------------------------------------------------------------------===//

/// @brief Maximum number of simultaneous playback voices.
/// @details Each playing sound instance consumes one voice. When all voices
///          are in use, new play requests will steal the oldest voice.
#ifndef VAUD_MAX_VOICES
#define VAUD_MAX_VOICES 32
#endif

/// @brief Maximum number of loaded sound effects.
/// @details Sound effects are fully loaded into memory. This limits the
///          total number of unique sounds that can be loaded simultaneously.
#ifndef VAUD_MAX_SOUNDS
#define VAUD_MAX_SOUNDS 256
#endif

/// @brief Maximum number of music streams.
/// @details Music is streamed from disk. Typically only 1-2 music tracks
///          play simultaneously (e.g., background music + jingle).
#ifndef VAUD_MAX_MUSIC
#define VAUD_MAX_MUSIC 4
#endif

//===----------------------------------------------------------------------===//
// Platform Detection
//===----------------------------------------------------------------------===//

#if defined(__APPLE__)
#define VAUD_PLATFORM_MACOS 1
#elif defined(__linux__)
#define VAUD_PLATFORM_LINUX 1
#elif defined(_WIN32)
#define VAUD_PLATFORM_WINDOWS 1
#else
#error "ZannaAUD: Unsupported platform"
#endif

//===----------------------------------------------------------------------===//
// Default Values
//===----------------------------------------------------------------------===//

/// @brief Default master volume (0.0 to 1.0).
#define VAUD_DEFAULT_MASTER_VOLUME 1.0f

/// @brief Default sound effect volume.
#define VAUD_DEFAULT_SOUND_VOLUME 1.0f

/// @brief Default music volume.
#define VAUD_DEFAULT_MUSIC_VOLUME 1.0f

/// @brief Default stereo pan (0.0 = center).
#define VAUD_DEFAULT_PAN 0.0f

//===----------------------------------------------------------------------===//
// Music Streaming Configuration
//===----------------------------------------------------------------------===//

/// @brief Size of music streaming buffer in frames.
/// @details Larger buffers reduce disk I/O frequency but increase memory usage.
///          8192 frames = ~186ms of audio per buffer.
#ifndef VAUD_MUSIC_BUFFER_FRAMES
#define VAUD_MUSIC_BUFFER_FRAMES 8192
#endif

/// @brief Number of streaming buffers for music (double/triple buffering).
#ifndef VAUD_MUSIC_BUFFER_COUNT
#define VAUD_MUSIC_BUFFER_COUNT 3
#endif

/// @brief Enable the per-context background music streaming thread (1 = on).
/// @details When enabled, vaud_create() starts one low-duty pump thread that
///          services music ring-buffer refills so streams keep playing even
///          when the app thread stalls between vaud_update() calls (asset
///          loads, long frames). Thread-creation failure is non-fatal: the
///          context still works with vaud_update() as the only refill pump.
#ifndef VAUD_STREAM_THREAD_ENABLE
#define VAUD_STREAM_THREAD_ENABLE 1
#endif

/// @brief Background streamer pump interval in milliseconds.
/// @details The music ring holds VAUD_MUSIC_BUFFER_COUNT x
///          VAUD_MUSIC_BUFFER_FRAMES frames (~557 ms at the defaults); a 50 ms
///          cadence refills far faster than realtime playback drains, leaving
///          an order-of-magnitude headroom while keeping the thread idle.
#ifndef VAUD_STREAM_THREAD_INTERVAL_MS
#define VAUD_STREAM_THREAD_INTERVAL_MS 50
#endif

#ifdef __cplusplus
}
#endif
