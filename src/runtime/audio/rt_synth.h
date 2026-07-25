//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_synth.h
// Purpose: Procedural sound synthesis — generates Sound objects from parameters
//          without requiring WAV files. Supports tone, sweep, noise, and preset
//          game sound effects.
//
// Key invariants:
//   - All generated sounds are 16-bit PCM mono at 44100 Hz.
//   - Returned Sound objects are identical to file-loaded sounds (same GC lifecycle).
//   - Duration is in milliseconds; frequencies in Hz (integer).
//   - Waveform types: 0=sine, 1=square, 2=sawtooth, 3=triangle.
//   - SFX preset types: 0=jump, 1=coin, 2=hit, 3=explosion, 4=powerup, 5=laser.
//
// Ownership/Lifetime:
//   - Returned Sound objects are GC-managed with refcount 1.
//   - Caller owns the reference and must release via Sound.Destroy.
//
// Links: rt_audio.h (sound playback), rt_soundbank.h (named registry)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares fixed-format procedural Sound generators and game-SFX presets.
/// @details Every generator produces caller-owned 44.1-kHz mono signed 16-bit
///          PCM through the standard runtime Sound wrapper. Numeric inputs are
///          clamped to bounded synthesis ranges, and no external audio asset is
///          required.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Oscillator waveform used by tone and sweep generation.
typedef enum {
    /// @brief Bhaskara-approximated sine wave.
    RT_WAVE_SINE = 0,
    /// @brief Equal-duty bipolar square wave.
    RT_WAVE_SQUARE = 1,
    /// @brief Rising bipolar sawtooth wave.
    RT_WAVE_SAWTOOTH = 2,
    /// @brief Bipolar triangle wave.
    RT_WAVE_TRIANGLE = 3,
} rt_wave_type_t;

/// @brief Built-in game-sound recipe selected by @ref rt_synth_sfx.
typedef enum {
    /// @brief Short ascending square-wave jump sweep.
    RT_SFX_JUMP = 0,
    /// @brief Two-stage high-pitched square-wave coin tone.
    RT_SFX_COIN = 1,
    /// @brief Short decaying noise hit.
    RT_SFX_HIT = 2,
    /// @brief Longer decaying noise explosion.
    RT_SFX_EXPLOSION = 3,
    /// @brief Ascending triangle-wave power-up sweep.
    RT_SFX_POWERUP = 4,
    /// @brief Descending sawtooth laser sweep.
    RT_SFX_LASER = 5,
} rt_sfx_preset_t;

/// @brief Generate a tone at a fixed frequency.
/// @param freq_hz Frequency in Hz (20-20000).
/// @param duration_ms Duration in milliseconds (1-10000).
/// @param waveform Waveform type (0=sine, 1=square, 2=saw, 3=triangle).
/// @return Caller-owned Sound object, or NULL on failure/audio unavailability.
void *rt_synth_tone(int64_t freq_hz, int64_t duration_ms, int64_t waveform);

/// @brief Generate a frequency sweep between two frequencies.
/// @details Frequency is interpolated linearly and a short edge envelope
///          prevents clicks.
/// @param start_hz Starting frequency clamped to `[20, 20000]` Hz.
/// @param end_hz Ending frequency clamped to `[20, 20000]` Hz.
/// @param duration_ms Duration clamped to `[1, 10000]` milliseconds.
/// @param waveform Waveform type clamped to @ref rt_wave_type_t values.
/// @return Caller-owned Sound object, or NULL on failure/audio unavailability.
void *rt_synth_sweep(int64_t start_hz, int64_t end_hz, int64_t duration_ms, int64_t waveform);

/// @brief Generate white noise.
/// @details Uses an LCG seeded from the runtime RNG with quadratic decay.
/// @param duration_ms Duration clamped to `[1, 10000]` milliseconds.
/// @param volume Volume level clamped to `[0, 100]`.
/// @return Caller-owned Sound object, or NULL on failure/audio unavailability.
void *rt_synth_noise(int64_t duration_ms, int64_t volume);

/// @brief Generate a preset game sound effect.
/// @param sfx_type SFX type (0=jump, 1=coin, 2=hit, 3=explosion, 4=powerup, 5=laser).
/// @return Caller-owned Sound object, or NULL for an unknown type/failure.
void *rt_synth_sfx(int64_t sfx_type);

#ifdef __cplusplus
}
#endif
