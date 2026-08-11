//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio_fx.h
// Purpose: Runtime-owned insert effects for integer audio mix groups.
// Key invariants:
//   - Effect ids are process-wide handles returned by the add functions; -1
//     signals invalid input or allocation failure.
//   - Public numeric parameters are sanitized/clamped so a bad Float cannot
//     poison filter state (frequencies/Q to valid ranges, peaking gain to
//     +/-40 dB, mix levels to [0, 1]).
// Ownership/Lifetime:
//   - Effect state is owned by the process-wide FX tables; Clear*/Remove
//     release it. No caller-visible allocations.
// Links: src/runtime/audio/rt_audio_fx.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the runtime-owned insert-effect registry for audio mix groups.
/// @details Add operations allocate ordered DSP nodes and return opaque positive
///          identifiers. Each group is capped at 32 inserts to bound callback
///          work and retained DSP memory. Mutation functions serialize access on the control
///          path. Mixer-facing query and processing functions are non-blocking:
///          under registry contention they report no active effects or leave
///          the sample block unchanged.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Append a low-pass biquad insert to @p group.
/// @param group Numeric mix-group index.
/// @param cutoff_hz Requested cutoff frequency in hertz.
/// @param q Requested resonance/quality factor.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_audio_fx_add_lowpass(int64_t group, double cutoff_hz, double q);

/// @brief Append a high-pass biquad insert to @p group.
/// @param group Numeric mix-group index.
/// @param cutoff_hz Requested cutoff frequency in hertz.
/// @param q Requested resonance/quality factor.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_audio_fx_add_highpass(int64_t group, double cutoff_hz, double q);

/// @brief Append a peaking equalizer insert.
/// @param group Numeric mix-group index.
/// @param freq_hz Requested center frequency in hertz.
/// @param q Requested bandwidth/quality factor.
/// @param gain_db Center-frequency gain, clamped to `[-40, 40]` decibels.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_audio_fx_add_peaking(int64_t group, double freq_hz, double q, double gain_db);

/// @brief Append a bounded feedback-delay insert.
/// @param group Numeric mix-group index.
/// @param delay_ms Requested delay duration in milliseconds.
/// @param feedback Requested feedback fraction, clamped to a stable range.
/// @param wet Requested wet-signal fraction, clamped to `[0, 1]`.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_audio_fx_add_delay(int64_t group, double delay_ms, double feedback, double wet);

/// @brief Append a stereo reverb insert.
/// @param group Numeric mix-group index.
/// @param room_size Requested normalized room-size parameter.
/// @param damping Requested normalized damping parameter.
/// @param wet Requested wet-signal fraction.
/// @return Positive effect identifier, or `-1` on failure.
int64_t rt_audio_fx_add_reverb(int64_t group, double room_size, double damping, double wet);

/// @brief Enable/disable an insert without removing it (unknown ids are no-ops).
/// @param group Numeric mix-group index containing the effect.
/// @param fx_id Positive identifier returned by an add operation.
/// @param bypass Non-zero to bypass processing; zero to enable it.
void rt_audio_fx_set_bypass(int64_t group, int64_t fx_id, int8_t bypass);

/// @brief Update a reverb insert's room/damping/wet in place (no reallocation).
/// @details Preserves the delay-line contents so eased parameter changes do not
///          reset the reverb tail.
/// @param group Numeric mix-group index containing the effect.
/// @param fx_id Positive identifier of an existing reverb insert.
/// @param room_size Requested normalized room-size parameter.
/// @param damping Requested normalized damping parameter.
/// @param wet Requested wet-signal fraction.
void rt_audio_fx_set_reverb_params(
    int64_t group, int64_t fx_id, double room_size, double damping, double wet);
/// @brief Remove one insert from @p group (unknown ids are no-ops).
/// @param group Numeric mix-group index containing the effect.
/// @param fx_id Positive identifier returned by an add operation.
void rt_audio_fx_remove(int64_t group, int64_t fx_id);

/// @brief Remove every insert from @p group.
/// @param group Numeric mix-group index to clear.
void rt_audio_fx_clear_group(int64_t group);

/// @brief Remove every insert from every group.
/// @details Releases all effect nodes and their delay/reverb storage.
void rt_audio_fx_clear_all(void);

/// @brief 1 when @p group has at least one active (non-bypassed) insert.
/// @details Uses a non-blocking registry query; lock contention returns zero.
/// @param group Numeric mix-group index to query.
/// @return Non-zero for an enabled chain; zero for invalid, empty, fully
///         bypassed, or momentarily contended state.
int rt_audio_fx_group_has_effects(int64_t group);

/// @brief Run @p group's insert chain over interleaved float samples in place.
/// @details Called from the mixer callback; non-finite outputs are sanitized
///          to silence per sample. Lock contention leaves the input unchanged,
///          and current DSP state operates on at most the first two channels.
/// @param group Numeric mix-group index to process.
/// @param samples Writable interleaved floating-point sample buffer.
/// @param frames Positive number of sample frames.
/// @param channels Positive channel count per frame.
/// @param sample_rate Backend sample rate in hertz; reserved for rate-aware DSP.
void rt_audio_fx_process_group(
    int64_t group, float *samples, int32_t frames, int32_t channels, int32_t sample_rate);

#ifdef __cplusplus
}
#endif
