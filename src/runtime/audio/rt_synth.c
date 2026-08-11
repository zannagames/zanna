//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_synth.c
// Purpose: Procedural sound synthesis — generates WAV data in memory and
//          loads it as a Sound object via rt_sound_load_mem. Supports tone
//          generation, frequency sweeps, noise, and preset game SFX.
//
// Key invariants:
//   - Output is always 16-bit PCM, mono, 44100 Hz.
//   - WAV data is built in a temporary heap buffer, loaded, then freed.
//   - Sine approximation uses Bhaskara I's rational formula (no libm dependency).
//   - All parameter values are clamped to safe ranges.
//
// Ownership/Lifetime:
//   - Temporary WAV buffers are malloc'd and freed within each function.
//   - Returned Sound objects are GC-managed with refcount 1.
//
// Links: rt_audio.h (rt_sound_load_mem), rt_synth.h (public API)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded procedural Sound synthesis without external assets.
/// @details Tone, linear sweep, decaying noise, and preset generators create
///          temporary 44.1-kHz mono signed 16-bit PCM, wrap it in a minimal WAV
///          image, and load it through the normal runtime Sound path. Temporary
///          sample/WAV storage is released before returning the caller-owned
///          Sound wrapper.

#include "rt_synth.h"
#include "rt_audio.h"
#include "rt_random.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

#define SYNTH_SAMPLE_RATE 44100
#define SYNTH_CHANNELS 1
#define SYNTH_BITS 16
#define SYNTH_MAX_AMP 32000 /* slightly below INT16_MAX to avoid clipping */
#define WAV_HEADER_SIZE 44

/* Pi approximation — sufficient for audio synthesis */
#define SYNTH_PI 3.14159265358979323846
#define SYNTH_2PI 6.28318530717958647692

//===----------------------------------------------------------------------===//
// Fixed-point sine approximation (no libm dependency)
//===----------------------------------------------------------------------===//

/// @brief Fast sine approximation using Bhaskara I's formula.
/// @param phase Phase in range [0.0, 1.0) representing [0, 2*PI).
/// @return Sine value in range [-1.0, 1.0].
static double synth_sin(double phase) {
    /* Map to [0, 2*PI) and use symmetry */
    double x = phase * SYNTH_2PI;

    /* Reduce to [0, PI] using sin(PI+x) = -sin(x) */
    int negate = 0;
    if (x > SYNTH_PI) {
        x -= SYNTH_PI;
        negate = 1;
    }

    /* Bhaskara I approximation: sin(x) ≈ 16x(PI-x) / (5*PI^2 - 4x(PI-x))
       Accurate to ~0.08% max error — inaudible for audio */
    double xpi = x * (SYNTH_PI - x);
    double result = 16.0 * xpi / (5.0 * SYNTH_PI * SYNTH_PI - 4.0 * xpi);

    return negate ? -result : result;
}

//===----------------------------------------------------------------------===//
// WAV Header Construction
//===----------------------------------------------------------------------===//

/// @brief Write a 16-bit little-endian integer into the WAV header buffer.
/// @param p Destination with at least two writable bytes.
/// @param v Value to encode.
static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/// @brief Write a 32-bit little-endian integer into the WAV header buffer.
/// @param p Destination with at least four writable bytes.
/// @param v Value to encode.
static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/// @brief Validate the sample count and compute WAV byte sizes without overflow.
/// @param num_samples Positive mono PCM sample count.
/// @param data_size_out Optional destination for PCM payload bytes.
/// @param wav_size_out Optional destination for header-plus-payload bytes.
/// @return Non-zero when both RIFF and platform sizes are representable.
static int synth_wav_sizes(int32_t num_samples, uint32_t *data_size_out, size_t *wav_size_out) {
    if (data_size_out)
        *data_size_out = 0;
    if (wav_size_out)
        *wav_size_out = 0;
    if (num_samples <= 0)
        return 0;

    uint64_t data_size = (uint64_t)(uint32_t)num_samples * (uint64_t)sizeof(int16_t);
    if (data_size > (uint64_t)UINT32_MAX - (uint64_t)(WAV_HEADER_SIZE - 8))
        return 0;

    if (data_size_out)
        *data_size_out = (uint32_t)data_size;
    if (wav_size_out)
        *wav_size_out = (size_t)WAV_HEADER_SIZE + (size_t)data_size;
    return 1;
}

/// @brief Write a minimal WAV header for mono 16-bit PCM data.
/// @param buf Destination with at least @ref WAV_HEADER_SIZE writable bytes.
/// @param data_size PCM payload length in bytes.
static void write_wav_header(uint8_t *buf, uint32_t data_size) {
    uint32_t file_size = (uint32_t)(WAV_HEADER_SIZE - 8) + data_size;
    int32_t byte_rate = SYNTH_SAMPLE_RATE * SYNTH_CHANNELS * (SYNTH_BITS / 8);
    int16_t block_align = SYNTH_CHANNELS * (SYNTH_BITS / 8);

    /* RIFF chunk */
    memcpy(buf + 0, "RIFF", 4);
    write_le32(buf + 4, (uint32_t)file_size);
    memcpy(buf + 8, "WAVE", 4);

    /* fmt sub-chunk */
    memcpy(buf + 12, "fmt ", 4);
    write_le32(buf + 16, 16u);
    write_le16(buf + 20, 1u); /* PCM */
    write_le16(buf + 22, (uint16_t)SYNTH_CHANNELS);
    write_le32(buf + 24, (uint32_t)SYNTH_SAMPLE_RATE);
    write_le32(buf + 28, (uint32_t)byte_rate);
    write_le16(buf + 32, (uint16_t)block_align);
    write_le16(buf + 34, (uint16_t)SYNTH_BITS);

    /* data sub-chunk */
    memcpy(buf + 36, "data", 4);
    write_le32(buf + 40, data_size);
}

//===----------------------------------------------------------------------===//
// Waveform Generation
//===----------------------------------------------------------------------===//

/// @brief Generate a single waveform sample at the given phase.
/// @param phase Normalized phase [0.0, 1.0).
/// @param waveform Waveform type constant.
/// @return Sample value in [-1.0, 1.0].
static double waveform_sample(double phase, int64_t waveform) {
    switch (waveform) {
        case RT_WAVE_SQUARE:
            return phase < 0.5 ? 1.0 : -1.0;

        case RT_WAVE_SAWTOOTH:
            return 2.0 * phase - 1.0;

        case RT_WAVE_TRIANGLE:
            if (phase < 0.25)
                return 4.0 * phase;
            else if (phase < 0.75)
                return 2.0 - 4.0 * phase;
            else
                return 4.0 * phase - 4.0;

        case RT_WAVE_SINE:
        default:
            return synth_sin(phase);
    }
}

/// @brief Advance an already-normalized oscillator phase without unbounded growth.
/// @param phase Current phase in `[0, 1)`.
/// @param increment Positive increment smaller than one cycle.
/// @return Advanced phase in `[0, 1)`.
static double synth_advance_phase(double phase, double increment) {
    phase += increment;
    if (phase >= 1.0)
        phase -= 1.0;
    return phase;
}

//===----------------------------------------------------------------------===//
// Sound Creation Helper
//===----------------------------------------------------------------------===//

/// @brief Build a WAV from PCM samples and load as a Sound object.
/// @details Copies samples into temporary WAV storage and does not retain the
///          input array.
/// @param samples Borrowed array of signed 16-bit mono PCM samples.
/// @param num_samples Positive sample count.
/// @return Caller-owned Sound object or NULL when audio is unavailable or
///         sizing, allocation, or loading fails.
static void *samples_to_sound(const int16_t *samples, int32_t num_samples) {
    if (!rt_audio_is_available())
        return NULL;
    if (!samples)
        return NULL;

    uint32_t data_size = 0;
    size_t wav_size = 0;
    if (!synth_wav_sizes(num_samples, &data_size, &wav_size))
        return NULL;

    uint8_t *wav_buf = (uint8_t *)malloc(wav_size);
    if (!wav_buf)
        return NULL;

    write_wav_header(wav_buf, data_size);
    for (int32_t i = 0; i < num_samples; ++i) {
        uint16_t sample = (uint16_t)samples[i];
        wav_buf[WAV_HEADER_SIZE + (size_t)i * 2] = (uint8_t)sample;
        wav_buf[WAV_HEADER_SIZE + (size_t)i * 2 + 1] = (uint8_t)(sample >> 8);
    }

    void *sound = rt_sound_load_mem(wav_buf, (int64_t)wav_size);
    free(wav_buf);

    return sound;
}

//===----------------------------------------------------------------------===//
// Clamp Helpers
//===----------------------------------------------------------------------===//

/// @brief Clamp @p v into the inclusive range `[lo, hi]`.
/// @param v Value to normalize.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return Clamped value.
static int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/// @brief Convert a duration in milliseconds to a checked sample count.
/// @details Performs the multiply in `int64_t`, rejects overflow and values
///          outside `int32_t`, and returns zero samples as failure. Public synth
///          entry points clamp duration before calling this helper.
/// @param duration_ms Duration in milliseconds.
/// @param out_samples Receives the computed sample count on success.
/// @return 1 when the sample count is valid, 0 otherwise.
static int synth_duration_to_samples(int64_t duration_ms, int32_t *out_samples) {
    if (!out_samples)
        return 0;
    *out_samples = 0;
    if (duration_ms <= 0 || duration_ms > INT64_MAX / SYNTH_SAMPLE_RATE)
        return 0;
    int64_t samples = (duration_ms * SYNTH_SAMPLE_RATE) / 1000;
    if (samples <= 0 || samples > INT32_MAX)
        return 0;
    *out_samples = (int32_t)samples;
    return 1;
}

/// @brief Compute the per-sample amplitude envelope for click-free playback.
/// @details Linear fade-in for the first @p fade_samples samples and a
///          mirror linear fade-out for the last @p fade_samples. Returns
///          1.0 in the steady-state middle and clamps to non-negative
///          values. Centralised here so tone/sweep/noise all share the
///          same envelope curve.
/// @param sample_index Current sample (0-indexed).
/// @param num_samples  Total samples in the rendered buffer.
/// @param fade_samples Length of each fade region in samples.
/// @return Amplitude scale in [0.0, 1.0].
static double synth_edge_envelope(int32_t sample_index, int32_t num_samples, int32_t fade_samples) {
    if (num_samples <= 1)
        return 0.0;
    if (fade_samples <= 0)
        return 1.0;

    if (fade_samples > num_samples - 1)
        fade_samples = num_samples - 1;

    double env = 1.0;
    if (sample_index < fade_samples)
        env = (double)sample_index / (double)fade_samples;
    if (sample_index >= num_samples - fade_samples) {
        double tail = (double)(num_samples - 1 - sample_index) / (double)fade_samples;
        if (tail < env)
            env = tail;
    }

    if (env < 0.0)
        env = 0.0;
    return env;
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// @brief Generate a constant-frequency tone as a playable Sound handle.
/// @details Synthesizes PCM samples at 44100 Hz using the selected waveform
///          (0=sine, 1=square, 2=sawtooth, 3=triangle). Applies 10ms fade-in/out
///          envelopes to prevent audible clicks.
/// @param freq_hz Frequency clamped to `[20, 20000]` hertz.
/// @param duration_ms Duration clamped to `[1, 10000]` milliseconds.
/// @param waveform Waveform identifier clamped to `[0, 3]`.
/// @return Caller-owned Sound handle, or NULL on allocation/loading failure.
void *rt_synth_tone(int64_t freq_hz, int64_t duration_ms, int64_t waveform) {
    if (!rt_audio_is_available())
        return NULL;

    freq_hz = clamp_i64(freq_hz, 20, 20000);
    duration_ms = clamp_i64(duration_ms, 1, 10000);
    waveform = clamp_i64(waveform, 0, 3);

    int32_t num_samples = 0;
    if (!synth_duration_to_samples(duration_ms, &num_samples))
        return NULL;

    int16_t *samples = (int16_t *)malloc((size_t)num_samples * sizeof(int16_t));
    if (!samples)
        return NULL;

    double phase = 0.0;
    double phase_inc = (double)freq_hz / (double)SYNTH_SAMPLE_RATE;
    int32_t fade_samples = SYNTH_SAMPLE_RATE / 100; /* 10ms */

    for (int32_t i = 0; i < num_samples; i++) {
        double val = waveform_sample(phase, waveform);

        /* Apply a short fade-in/fade-out to avoid clicks (10ms each) */
        double env = synth_edge_envelope(i, num_samples, fade_samples);

        samples[i] = (int16_t)(val * env * SYNTH_MAX_AMP);
        phase = synth_advance_phase(phase, phase_inc);
    }

    void *sound = samples_to_sound(samples, num_samples);
    free(samples);
    return sound;
}

/// @brief Generate a frequency sweep (glissando) from start_hz to end_hz as a playable Sound.
/// @details Linear frequency interpolation with fade envelopes. Useful for laser,
///          power-up, and sci-fi sound effects.
/// @param start_hz Initial frequency clamped to `[20, 20000]` hertz.
/// @param end_hz Final frequency clamped to `[20, 20000]` hertz.
/// @param duration_ms Duration clamped to `[1, 10000]` milliseconds.
/// @param waveform Waveform identifier clamped to `[0, 3]`.
/// @return Caller-owned Sound handle, or NULL on allocation/loading failure.
void *rt_synth_sweep(int64_t start_hz, int64_t end_hz, int64_t duration_ms, int64_t waveform) {
    if (!rt_audio_is_available())
        return NULL;

    start_hz = clamp_i64(start_hz, 20, 20000);
    end_hz = clamp_i64(end_hz, 20, 20000);
    duration_ms = clamp_i64(duration_ms, 1, 10000);
    waveform = clamp_i64(waveform, 0, 3);

    int32_t num_samples = 0;
    if (!synth_duration_to_samples(duration_ms, &num_samples))
        return NULL;

    int16_t *samples = (int16_t *)malloc((size_t)num_samples * sizeof(int16_t));
    if (!samples)
        return NULL;

    double phase = 0.0;
    int32_t fade_samples = SYNTH_SAMPLE_RATE / 100;
    double interpolation_denominator = num_samples > 1 ? (double)(num_samples - 1) : 1.0;

    for (int32_t i = 0; i < num_samples; i++) {
        /* Linear frequency interpolation */
        double t = (double)i / interpolation_denominator;
        double freq = (double)start_hz + ((double)end_hz - (double)start_hz) * t;
        double phase_inc = freq / (double)SYNTH_SAMPLE_RATE;

        double val = waveform_sample(phase, waveform);

        /* Fade envelope */
        double env = synth_edge_envelope(i, num_samples, fade_samples);

        samples[i] = (int16_t)(val * env * SYNTH_MAX_AMP);
        phase = synth_advance_phase(phase, phase_inc);
    }

    void *sound = samples_to_sound(samples, num_samples);
    free(samples);
    return sound;
}

/// @brief Generate white noise as a playable Sound handle (LCG PRNG, fade envelopes).
/// @details Applies a quadratic decay and short edge envelope to deterministic
///          LCG samples seeded from the runtime RNG.
/// @param duration_ms Duration clamped to `[1, 10000]` milliseconds.
/// @param volume Amplitude percentage clamped to `[0, 100]`.
/// @return Caller-owned Sound handle, or NULL on allocation/loading failure.
void *rt_synth_noise(int64_t duration_ms, int64_t volume) {
    if (!rt_audio_is_available())
        return NULL;

    duration_ms = clamp_i64(duration_ms, 1, 10000);
    volume = clamp_i64(volume, 0, 100);

    int32_t num_samples = 0;
    if (!synth_duration_to_samples(duration_ms, &num_samples))
        return NULL;

    int16_t *samples = (int16_t *)malloc((size_t)num_samples * sizeof(int16_t));
    if (!samples)
        return NULL;

    /* Simple LCG PRNG seeded from the runtime RNG (no external dependency). */
    uint32_t rng_state = (uint32_t)rt_rand_range(1, INT_MAX);
    double vol_scale = (double)volume / 100.0;
    double decay_denominator = num_samples > 1 ? (double)(num_samples - 1) : 1.0;
    int32_t fade_samples = SYNTH_SAMPLE_RATE / 200;

    for (int32_t i = 0; i < num_samples; i++) {
        /* LCG: state = state * 1103515245 + 12345 */
        rng_state = rng_state * 1103515245u + 12345u;
        uint32_t high_word = rng_state >> 16;
        int32_t noise_val =
            high_word <= INT16_MAX ? (int32_t)high_word : (int32_t)high_word - 65536;

        /* Quadratic decay envelope */
        double t = (double)i / decay_denominator;
        double env = 1.0 - t; /* Linear decay */
        env = env * env;      /* Quadratic decay for more natural sound */
        env *= synth_edge_envelope(i, num_samples, fade_samples);

        samples[i] = (int16_t)((double)noise_val * env * vol_scale);
    }

    void *sound = samples_to_sound(samples, num_samples);
    free(samples);
    return sound;
}

//===----------------------------------------------------------------------===//
// Preset SFX
//===----------------------------------------------------------------------===//

/// @brief Generate "jump" sound: quick ascending frequency sweep.
/// @return Caller-owned preset Sound, or NULL on failure.
static void *sfx_jump(void) {
    return rt_synth_sweep(200, 600, 150, RT_WAVE_SQUARE);
}

/// @brief Generate "coin" sound: two quick high-pitched tones.
/// @return Caller-owned preset Sound, or NULL on failure.
static void *sfx_coin(void) {
    /* Two-tone coin: 880Hz + 1760Hz, 80ms total */
    int32_t half = (int32_t)(80 * SYNTH_SAMPLE_RATE / 1000) / 2;
    int32_t num_samples = half * 2;

    int16_t *samples = (int16_t *)malloc((size_t)num_samples * sizeof(int16_t));
    if (!samples)
        return NULL;

    double phase = 0.0;
    for (int32_t i = 0; i < num_samples; i++) {
        double freq = (i < half) ? 880.0 : 1760.0;
        double phase_inc = freq / (double)SYNTH_SAMPLE_RATE;

        double val = waveform_sample(phase, RT_WAVE_SQUARE);

        /* Tiny amplitude to keep it crisp */
        double env = 0.6;
        /* Quick fade at boundaries */
        int32_t fade = SYNTH_SAMPLE_RATE / 200; /* 5ms */
        env *= synth_edge_envelope(i, num_samples, fade);

        samples[i] = (int16_t)(val * env * SYNTH_MAX_AMP);
        phase = synth_advance_phase(phase, phase_inc);

        /* Reset phase at tone boundary for clean transition */
        if (i == half - 1)
            phase = 0.0;
    }

    void *sound = samples_to_sound(samples, num_samples);
    free(samples);
    return sound;
}

/// @brief Generate "hit" sound: short noise burst with fast decay.
/// @return Caller-owned preset Sound, or NULL on failure.
static void *sfx_hit(void) {
    return rt_synth_noise(80, 90);
}

/// @brief Generate "explosion" sound: longer noise with slow decay.
/// @return Caller-owned preset Sound, or NULL on failure.
static void *sfx_explosion(void) {
    return rt_synth_noise(500, 100);
}

/// @brief Generate "powerup" sound: ascending sweep with triangle wave.
/// @return Caller-owned preset Sound, or NULL on failure.
static void *sfx_powerup(void) {
    return rt_synth_sweep(300, 1200, 400, RT_WAVE_TRIANGLE);
}

/// @brief Generate "laser" sound: quick descending sweep with sawtooth.
/// @return Caller-owned preset Sound, or NULL on failure.
static void *sfx_laser(void) {
    return rt_synth_sweep(1500, 200, 120, RT_WAVE_SAWTOOTH);
}

/// @brief Public SFX dispatcher — pick the preset generator for @p sfx_type.
/// @details Thin switch that maps @ref rt_sfx_preset_t to the
///          corresponding `sfx_*` helper. Returns NULL for unknown
///          types so callers can detect bad input.
/// @param sfx_type Preset identifier from @ref rt_sfx_preset_t.
/// @return Caller-owned preset Sound, or NULL for an unknown type/failure.
void *rt_synth_sfx(int64_t sfx_type) {
    if (!rt_audio_is_available())
        return NULL;

    switch (sfx_type) {
        case RT_SFX_JUMP:
            return sfx_jump();
        case RT_SFX_COIN:
            return sfx_coin();
        case RT_SFX_HIT:
            return sfx_hit();
        case RT_SFX_EXPLOSION:
            return sfx_explosion();
        case RT_SFX_POWERUP:
            return sfx_powerup();
        case RT_SFX_LASER:
            return sfx_laser();
        default:
            return NULL;
    }
}
