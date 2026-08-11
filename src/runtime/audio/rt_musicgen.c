//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_musicgen.c
// Purpose: Procedural music composition — tracker-style sequencer that pre-
//          renders multi-channel songs with ADSR envelopes and chiptune effects
//          into a Sound object via rt_sound_load_mem. Zero external dependencies.
//
// Key invariants:
//   - Output is always 16-bit stereo PCM at 44100 Hz.
//   - WAV data is built in a temporary heap buffer, loaded, then freed.
//   - Sine approximation uses Bhaskara I's formula (no libm dependency).
//   - All parameter values are clamped to safe ranges.
//   - Notes are sorted by beat position before rendering (required for
//     portamento which needs the previous note's frequency).
//   - The 32-bit stereo accumulator prevents clipping during channel mixing.
//
// Ownership/Lifetime:
//   - Song builder allocated via rt_obj_new_i64, no finalizer (pure data).
//   - Temporary PCM/WAV buffers are malloc'd and freed within Build().
//   - Returned Sound objects are GC-managed with refcount 1.
//
// Links: rt_musicgen.h (public API), rt_audio.h (rt_sound_load_mem),
//        rt_synth.c (reference for waveform/WAV patterns)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the procedural tracker-style music builder and renderer.
/// @details Songs contain bounded channel/note arrays and render ahead into
///          44.1-kHz stereo 16-bit PCM. Rendering applies waveform synthesis,
///          ADSR, pan, detune, modulation, arpeggio, portamento, swing, and
///          optional loop-boundary crossfade before wrapping the temporary WAV
///          image as a runtime Sound handle.

#include "rt_musicgen.h"
#include "rt_audio.h"
#include "rt_object.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

#define MG_SAMPLE_RATE 44100
#define MG_CHANNELS 2 /* stereo output */
#define MG_BITS 16
#define MG_MAX_AMP 30000 /* below INT16_MAX to leave headroom */
#define MG_WAV_HEADER 44
#define MG_MAX_DURATION_S (5 * 60) /* 5 minutes */
#define MG_CROSSFADE_MS 10         /* loop crossfade duration */
#define MG_CLASS_ID INT64_C(-0x730103)

#define MG_PI 3.14159265358979323846
#define MG_2PI 6.28318530717958647692

//===----------------------------------------------------------------------===//
// Internal Data Structures
//===----------------------------------------------------------------------===//

typedef struct {
    int64_t attack_ms;
    int64_t decay_ms;
    int64_t sustain_pct;
    int64_t release_ms;
} mg_envelope_t;

typedef struct {
    int64_t beat_pos;
    int64_t midi_note;
    int64_t duration;
    int64_t velocity;
    int64_t order;
} mg_note_t;

typedef struct {
    int64_t waveform;
    mg_envelope_t envelope;
    mg_note_t notes[MUSICGEN_MAX_NOTES];
    int32_t note_count;

    /* Basic settings */
    int64_t volume;
    int64_t duty_cycle;
    int64_t pan;
    int64_t detune_cents;

    /* Effects */
    int64_t vibrato_depth;
    int64_t vibrato_speed;
    int64_t tremolo_depth;
    int64_t tremolo_speed;
    int64_t arp_semi1;
    int64_t arp_semi2;
    int64_t arp_speed;
    int64_t portamento_ms;
} mg_channel_t;

typedef struct {
    void *vptr;
    int64_t bpm;
    int64_t length_centbeats;
    int64_t swing;
    int32_t loopable;
    mg_channel_t channels[MUSICGEN_MAX_CHANNELS];
    int32_t channel_count;
} mg_song_t;

//===----------------------------------------------------------------------===//
// Clamping Helpers
//===----------------------------------------------------------------------===//

/// @brief Clamp an integer into an inclusive range.
/// @param v Value to normalize.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return @p v limited to `[@p lo, @p hi]`.
static int64_t mg_clamp(int64_t v, int64_t lo, int64_t hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/// @brief Compute the maximum song length in centbeats for a given tempo.
/// @details Derived from `MG_MAX_DURATION_S` (the hard cap on rendered
///          audio length): `frames = beats × samples_per_beat`, so the
///          centbeat ceiling is `MG_MAX_DURATION_S × bpm × 100 / 60`.
///          Used by `rt_musicgen_set_length` and `rt_musicgen_add_note`
///          so notes/lengths past this threshold are clipped.
/// @param bpm Beats-per-minute (clamped to 20..300 to match builder limits).
/// @return Maximum legal centbeat position for that tempo.
static int64_t mg_max_centbeats_for_bpm(int64_t bpm) {
    bpm = mg_clamp(bpm, 20, 300);
    return ((int64_t)MG_MAX_DURATION_S * bpm * 100) / 60;
}

/// @brief Convert a centbeat position to a sample-frame count.
/// @details Multiplies by `samples_per_beat / 100`, but does the
///          multiply in int64 and rejects overflow so the result fits.
///          Negative centbeats clamp to zero (used pervasively in
///          render-time loops where negative timing would walk off the
///          PCM buffer).
/// @param centbeats        Beat position (100 = 1 beat).
/// @param samples_per_beat Frames per beat (`SAMPLE_RATE × 60 / bpm`).
/// @param out_frames       Receives the converted frame count on success.
/// @return 1 on success, 0 on bad inputs or multiplication overflow.
static int mg_centbeats_to_frames(int64_t centbeats,
                                  int32_t samples_per_beat,
                                  int64_t *out_frames) {
    if (!out_frames)
        return 0;
    *out_frames = 0;
    if (samples_per_beat <= 0)
        return 0;
    if (centbeats < 0)
        centbeats = 0;
    if (centbeats > INT64_MAX / (int64_t)samples_per_beat)
        return 0;
    *out_frames = (centbeats * (int64_t)samples_per_beat) / 100;
    return 1;
}

/// @brief Safe-cast an opaque handle to mg_song_t.
/// @param song_ptr Opaque runtime object to validate.
/// @return The song, or NULL if @p song_ptr is not a MusicGen song object.
static mg_song_t *mg_as_song(void *song_ptr) {
    if (!rt_obj_is_instance(song_ptr, MG_CLASS_ID, sizeof(mg_song_t)))
        return NULL;
    return (mg_song_t *)song_ptr;
}

//===----------------------------------------------------------------------===//
// Sine Approximation (no libm — identical to rt_synth.c)
//===----------------------------------------------------------------------===//

/// @brief Approximate sine without the platform math library.
/// @details Maps an already-normalized phase to `[0, 2π)` and applies Bhaskara
///          I's approximation with approximately 0.08% maximum error.
/// @param phase Wave-cycle phase, where one unit is a complete cycle.
/// @return Approximate sine value in `[-1, 1]`.
static double mg_sin(double phase) {
    double x = phase * MG_2PI;
    int negate = 0;
    if (x > MG_PI) {
        x -= MG_PI;
        negate = 1;
    }

    double xpi = x * (MG_PI - x);
    double result = 16.0 * xpi / (5.0 * MG_PI * MG_PI - 4.0 * xpi);

    return negate ? -result : result;
}

//===----------------------------------------------------------------------===//
// MIDI-to-Frequency Table (128 entries, no libm)
//===----------------------------------------------------------------------===//

/// Pre-computed frequencies for MIDI notes 0-127.
/// Formula: freq = 440 * 2^((note - 69) / 12)
static const double midi_freq[128] = {
    /* C-1  to B-1  (MIDI 0-11) */
    8.17580,
    8.66196,
    9.17702,
    9.72272,
    10.30086,
    10.91338,
    11.56233,
    12.24986,
    12.97827,
    13.75000,
    14.56762,
    15.43385,
    /* C0   to B0   (MIDI 12-23) */
    16.35160,
    17.32391,
    18.35405,
    19.44544,
    20.60172,
    21.82676,
    23.12465,
    24.49971,
    25.95654,
    27.50000,
    29.13524,
    30.86771,
    /* C1   to B1   (MIDI 24-35) */
    32.70320,
    34.64783,
    36.70810,
    38.89087,
    41.20344,
    43.65353,
    46.24930,
    48.99943,
    51.91309,
    55.00000,
    58.27047,
    61.73541,
    /* C2   to B2   (MIDI 36-47) */
    65.40639,
    69.29566,
    73.41619,
    77.78175,
    82.40689,
    87.30706,
    92.49861,
    97.99886,
    103.82617,
    110.00000,
    116.54094,
    123.47083,
    /* C3   to B3   (MIDI 48-59) */
    130.81278,
    138.59132,
    146.83238,
    155.56349,
    164.81378,
    174.61412,
    184.99721,
    195.99772,
    207.65235,
    220.00000,
    233.08188,
    246.94165,
    /* C4   to B4   (MIDI 60-71) */
    261.62557,
    277.18263,
    293.66477,
    311.12698,
    329.62756,
    349.22823,
    369.99442,
    391.99544,
    415.30470,
    440.00000,
    466.16376,
    493.88330,
    /* C5   to B5   (MIDI 72-83) */
    523.25113,
    554.36526,
    587.32954,
    622.25397,
    659.25511,
    698.45646,
    739.98885,
    783.99087,
    830.60940,
    880.00000,
    932.32752,
    987.76660,
    /* C6   to B6   (MIDI 84-95) */
    1046.50226,
    1108.73052,
    1174.65907,
    1244.50793,
    1318.51023,
    1396.91293,
    1479.97769,
    1567.98174,
    1661.21879,
    1760.00000,
    1864.65505,
    1975.53321,
    /* C7   to B7   (MIDI 96-107) */
    2093.00452,
    2217.46105,
    2349.31814,
    2489.01587,
    2637.02046,
    2793.82585,
    2959.95538,
    3135.96349,
    3322.43758,
    3520.00000,
    3729.31009,
    3951.06641,
    /* C8   to B8   (MIDI 108-119) */
    4186.00904,
    4434.92210,
    4698.63629,
    4978.03174,
    5274.04091,
    5587.65170,
    5919.91076,
    6271.92698,
    6644.87516,
    7040.00000,
    7458.62018,
    7902.13282,
    /* C9   to G9   (MIDI 120-127) */
    8372.01809,
    8869.84419,
    9397.27257,
    9956.06348,
    10548.08182,
    11175.30341,
    11839.82153,
    12543.85306};

//===----------------------------------------------------------------------===//
// pow2_cents — Pitch Offset Helper (no libm)
//===----------------------------------------------------------------------===//

/// Pre-computed 2^(i/12) for i=0..12 (semitone ratios within one octave).
static const double semitone_ratio[13] = {
    1.00000000000, /* 0  */
    1.05946309436, /* 1  */
    1.12246204831, /* 2  */
    1.18920711500, /* 3  */
    1.25992104989, /* 4  */
    1.33483985417, /* 5  */
    1.41421356237, /* 6  */
    1.49830707688, /* 7  */
    1.58740105197, /* 8  */
    1.68179283051, /* 9  */
    1.78179743628, /* 10 */
    1.88774862536, /* 11 */
    2.00000000000  /* 12 */
};

/// @brief Compute 2^(cents/1200) without libm.
/// @param cents Pitch offset in cents (-2400 to +2400).
/// @return Frequency multiplier.
static double pow2_cents(int64_t cents) {
    if (cents == 0)
        return 1.0;

    /* Handle negative values via reciprocal */
    int negate = 0;
    uint64_t magnitude;
    if (cents < 0) {
        negate = 1;
        magnitude = (uint64_t)(-(cents + 1)) + 1u;
    } else {
        magnitude = (uint64_t)cents;
    }
    if (magnitude > 2400u)
        magnitude = 2400u;

    /* Decompose: cents = semitones * 100 + remainder */
    uint64_t semitones = magnitude / 100u;
    uint64_t remainder = magnitude % 100u;

    /* Octave component: 2^(semitones/12) */
    /* Split into octaves and sub-octave semitones */
    uint64_t octaves = semitones / 12u;
    uint64_t sub_semi = semitones % 12u;

    /* Start with octave power (exact powers of 2) */
    double result = 1.0;
    for (uint64_t i = 0; i < octaves; i++)
        result *= 2.0;

    /* Apply sub-octave semitone from table */
    result *= semitone_ratio[sub_semi];

    /* Apply fractional cents via linear interpolation between semitone ratios */
    if (remainder > 0) {
        double lo = semitone_ratio[sub_semi];
        double hi = semitone_ratio[sub_semi + 1];
        /* Linear interp: approximate 2^(remainder/1200) */
        double frac = (double)remainder / 100.0;
        /* We already applied lo via result, so scale by (hi/lo)^frac ≈ 1 + frac*(hi/lo - 1) */
        double ratio = hi / lo;
        result *= 1.0 + frac * (ratio - 1.0);
    }

    return negate ? (1.0 / result) : result;
}

//===----------------------------------------------------------------------===//
// Waveform Generation
//===----------------------------------------------------------------------===//

/// @brief Generate a waveform sample with duty cycle support.
/// @param phase Normalized phase [0.0, 1.0).
/// @param waveform Waveform type (0-3). Noise handled separately.
/// @param duty Duty cycle percentage (0-100, only for square wave).
/// @return Sample value in [-1.0, 1.0].
static double mg_waveform(double phase, int64_t waveform, int64_t duty) {
    switch (waveform) {
        case MUSICGEN_WAVE_SQUARE: {
            double threshold = (double)duty / 100.0;
            if (threshold < 0.01)
                threshold = 0.01;
            if (threshold > 0.99)
                threshold = 0.99;
            return phase < threshold ? 1.0 : -1.0;
        }

        case MUSICGEN_WAVE_SAWTOOTH:
            return 2.0 * phase - 1.0;

        case MUSICGEN_WAVE_TRIANGLE:
            if (phase < 0.25)
                return 4.0 * phase;
            else if (phase < 0.75)
                return 2.0 - 4.0 * phase;
            else
                return 4.0 * phase - 4.0;

        case MUSICGEN_WAVE_SINE:
        default:
            return mg_sin(phase);
    }
}

/// @brief Advance a normalized oscillator phase and wrap at one cycle.
/// @param phase Current phase in `[0, 1)`.
/// @param increment Non-negative increment smaller than one cycle.
/// @return Advanced phase in `[0, 1)`.
static double mg_advance_phase(double phase, double increment) {
    phase += increment;
    if (phase >= 1.0)
        phase -= 1.0;
    return phase;
}

//===----------------------------------------------------------------------===//
// ADSR Envelope
//===----------------------------------------------------------------------===//

/// @brief Calculate attack/decay/sustain amplitude at a time after note-on.
/// @param env Envelope parameters.
/// @param t_s Elapsed time since note-on, in seconds.
/// @return Amplitude multiplier in [0.0, 1.0].
static double mg_adsr_note_level_at(const mg_envelope_t *env, double t_s) {
    double atk_s = (double)env->attack_ms / 1000.0;
    double dec_s = (double)env->decay_ms / 1000.0;
    double sus = (double)env->sustain_pct / 100.0;

    if (t_s < atk_s)
        return (atk_s > 0.0) ? (t_s / atk_s) : 1.0;

    t_s -= atk_s;
    if (t_s < dec_s) {
        double decay_t = (dec_s > 0.0) ? (t_s / dec_s) : 1.0;
        return 1.0 - (1.0 - sus) * decay_t;
    }

    return sus;
}

/// @brief Sample the full ADSR envelope including the release tail.
/// @details Composition of the attack/decay/sustain segments from
///          @ref mg_adsr_note_level_at with a linear release that
///          interpolates from `note_dur_s`'s instantaneous level down
///          to zero over `env->release_ms`. Beyond `note_dur_s +
///          release_s` the function returns 0.0 so the per-channel
///          accumulator stops adding silence.
/// @param env              Envelope parameters.
/// @param sample_offset    Frames since note-on.
/// @param note_dur_samples Note duration (sustain length) in frames.
/// @return Amplitude multiplier in [0.0, 1.0].
static double mg_adsr(const mg_envelope_t *env, int32_t sample_offset, int32_t note_dur_samples) {
    double t_s = (double)sample_offset / (double)MG_SAMPLE_RATE;
    double rel_s = (double)env->release_ms / 1000.0;
    double note_dur_s = (double)note_dur_samples / (double)MG_SAMPLE_RATE;

    /* Sustain phase — hold until note-off */
    if (t_s < note_dur_s)
        return mg_adsr_note_level_at(env, t_s);

    /* Release phase */
    double rel_t = t_s - note_dur_s;
    if (rel_t >= rel_s)
        return 0.0;

    double release_start = mg_adsr_note_level_at(env, note_dur_s);
    return (rel_s > 0.0) ? (release_start * (1.0 - rel_t / rel_s)) : 0.0;
}

//===----------------------------------------------------------------------===//
// Noise Generator
//===----------------------------------------------------------------------===//

/// Simple LCG PRNG state for deterministic noise.
typedef struct {
    uint32_t state;
    double prev_sample; /* for one-pole lowpass filter */
} mg_noise_t;

/// @brief Initialize noise generator with a seed.
/// @param n Noise/filter state to initialize.
/// @param seed Deterministic initial linear-congruential state.
static void mg_noise_init(mg_noise_t *n, uint32_t seed) {
    if (!n)
        return;
    n->state = seed;
    n->prev_sample = 0.0;
}

/// @brief Generate filtered noise sample.
/// @param n Noise state.
/// @param cutoff_freq Lowpass cutoff frequency in Hz.
/// @return Sample in [-1.0, 1.0].
static double mg_noise_sample(mg_noise_t *n, double cutoff_freq) {
    if (!n)
        return 0.0;
    if (cutoff_freq < 1.0)
        cutoff_freq = 1.0;
    if (cutoff_freq > (double)MG_SAMPLE_RATE * 0.5)
        cutoff_freq = (double)MG_SAMPLE_RATE * 0.5;

    /* LCG step */
    n->state = n->state * 1103515245u + 12345u;
    uint32_t high_word = n->state >> 16;
    int32_t signed_word = high_word <= INT16_MAX ? (int32_t)high_word : (int32_t)high_word - 65536;
    double white = (double)signed_word / 32768.0;

    /* One-pole lowpass: y[n] = y[n-1] + alpha * (x[n] - y[n-1])
       alpha = dt / (RC + dt), where RC = 1/(2*PI*cutoff) */
    double dt = 1.0 / (double)MG_SAMPLE_RATE;
    double rc = 1.0 / (MG_2PI * cutoff_freq);
    double alpha = dt / (rc + dt);

    /* Clamp alpha for stability */
    if (alpha > 1.0)
        alpha = 1.0;
    if (alpha < 0.001)
        alpha = 0.001;

    n->prev_sample = n->prev_sample + alpha * (white - n->prev_sample);
    return n->prev_sample;
}

//===----------------------------------------------------------------------===//
// Note Sorting (for portamento)
//===----------------------------------------------------------------------===//

/// @brief Compare notes by beat position, then insertion order for qsort.
/// @param a Pointer to the first @ref mg_note_t.
/// @param b Pointer to the second @ref mg_note_t.
/// @return Negative, zero, or positive according to stable render order.
static int mg_note_compare(const void *a, const void *b) {
    const mg_note_t *na = (const mg_note_t *)a;
    const mg_note_t *nb = (const mg_note_t *)b;
    if (na->beat_pos < nb->beat_pos)
        return -1;
    if (na->beat_pos > nb->beat_pos)
        return 1;
    if (na->order < nb->order)
        return -1;
    if (na->order > nb->order)
        return 1;
    return 0;
}

//===----------------------------------------------------------------------===//
// WAV Header (stereo variant)
//===----------------------------------------------------------------------===//

/// @brief Write a 16-bit little-endian integer to a byte buffer.
/// @param p Destination with space for two bytes.
/// @param v Value to encode.
static void mg_write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/// @brief Write a 32-bit little-endian integer to a byte buffer.
/// @param p Destination with space for four bytes.
/// @param v Value to encode.
static void mg_write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/// @brief Validate frame count and compute WAV byte sizes without signed overflow.
/// @param num_frames Positive stereo PCM frame count.
/// @param data_size_out Optional destination for PCM payload bytes.
/// @param wav_size_out Optional destination for total header-plus-payload bytes.
/// @return Non-zero when the sizes are representable in RIFF and `size_t`.
static int mg_wav_sizes(int32_t num_frames, uint32_t *data_size_out, size_t *wav_size_out) {
    if (data_size_out)
        *data_size_out = 0;
    if (wav_size_out)
        *wav_size_out = 0;
    if (num_frames <= 0)
        return 0;

    uint64_t data_size =
        (uint64_t)(uint32_t)num_frames * (uint64_t)MG_CHANNELS * (uint64_t)(MG_BITS / 8);
    if (data_size > (uint64_t)UINT32_MAX - (uint64_t)(MG_WAV_HEADER - 8))
        return 0;

    if (data_size_out)
        *data_size_out = (uint32_t)data_size;
    if (wav_size_out)
        *wav_size_out = (size_t)MG_WAV_HEADER + (size_t)data_size;
    return 1;
}

/// @brief Write a WAV header for stereo 16-bit PCM data.
/// @param buf Destination with at least @ref MG_WAV_HEADER writable bytes.
/// @param data_size PCM payload length in bytes.
static void mg_write_wav_header(uint8_t *buf, uint32_t data_size) {
    uint32_t file_size = (uint32_t)(MG_WAV_HEADER - 8) + data_size;
    int32_t byte_rate = MG_SAMPLE_RATE * MG_CHANNELS * (MG_BITS / 8);
    int16_t block_align = MG_CHANNELS * (MG_BITS / 8);

    memcpy(buf + 0, "RIFF", 4);
    mg_write_le32(buf + 4, (uint32_t)file_size);
    memcpy(buf + 8, "WAVE", 4);

    memcpy(buf + 12, "fmt ", 4);
    mg_write_le32(buf + 16, 16u);
    mg_write_le16(buf + 20, 1u); /* PCM */
    mg_write_le16(buf + 22, (uint16_t)MG_CHANNELS);
    mg_write_le32(buf + 24, (uint32_t)MG_SAMPLE_RATE);
    mg_write_le32(buf + 28, (uint32_t)byte_rate);
    mg_write_le16(buf + 32, (uint16_t)block_align);
    mg_write_le16(buf + 34, (uint16_t)MG_BITS);

    memcpy(buf + 36, "data", 4);
    mg_write_le32(buf + 40, data_size);
}

//===----------------------------------------------------------------------===//
// Render a Single Note into Accumulator
//===----------------------------------------------------------------------===//

/// Per-channel rendering state tracked across notes.
typedef struct {
    double prev_freq; /* last note's target frequency (for portamento) */
    int64_t prev_beat_pos;
} mg_render_state_t;

/// @brief Add @p value to `*dst` with int32 saturation (no wrap).
/// @details Performed in int64 to avoid the standard
///          implementation-defined behaviour of signed int32 overflow.
///          Used per-sample so an arrangement that piles up many
///          loud channels degrades gracefully into clipping rather
///          than into wrap-around glitches.
/// @param dst Accumulator sample to update.
/// @param value Signed contribution to add.
static void mg_accum_add_saturated(int32_t *dst, int32_t value) {
    int64_t sum = (int64_t)*dst + (int64_t)value;
    if (sum > INT32_MAX)
        sum = INT32_MAX;
    else if (sum < INT32_MIN)
        sum = INT32_MIN;
    *dst = (int32_t)sum;
}

/// @brief Render one note's audio into the stereo accumulator.
/// @param accum 32-bit stereo interleaved accumulator.
/// @param total_frames Total frames in the accumulator.
/// @param note The note to render.
/// @param chan The channel configuration.
/// @param samples_per_beat Samples per beat (derived from BPM).
/// @param swing Swing amount (0-100).
/// @param state Per-channel render state (portamento).
/// @param channel_count Number of active channels (for gain division).
static void mg_render_note(int32_t *accum,
                           int32_t total_frames,
                           const mg_note_t *note,
                           const mg_channel_t *chan,
                           int32_t samples_per_beat,
                           int64_t swing,
                           mg_render_state_t *state,
                           int32_t channel_count) {
    /* Calculate note timing */
    int64_t start64 = 0;
    if (!mg_centbeats_to_frames(note->beat_pos, samples_per_beat, &start64))
        return;

    /* Apply swing: shift notes on off-beats (at half-beat boundaries) */
    if (swing > 0) {
        int64_t half_beat = 50; /* centbeats */
        /* Check if this note falls on an odd half-beat */
        int64_t half_beats = note->beat_pos / half_beat;
        if ((half_beats % 2) == 1) {
            int64_t shift = (swing * (int64_t)samples_per_beat / 2) / 100;
            if (start64 > INT64_MAX - shift)
                return;
            start64 += shift;
        }
    }

    int64_t dur64 = 0;
    if (!mg_centbeats_to_frames(note->duration, samples_per_beat, &dur64))
        return;
    if (dur64 < 1)
        dur64 = 1;
    int64_t rel_samples64 = (chan->envelope.release_ms * (int64_t)MG_SAMPLE_RATE) / 1000;
    if (start64 >= total_frames)
        return;
    if (dur64 > INT64_MAX - rel_samples64 || start64 > INT64_MAX - dur64 - rel_samples64)
        return;

    int64_t end64 = start64 + dur64 + rel_samples64;
    if (end64 > total_frames)
        end64 = total_frames;
    if (end64 <= start64)
        return;

    int32_t start = (int32_t)start64;
    int32_t end = (int32_t)end64;
    int32_t dur = (dur64 > INT32_MAX) ? INT32_MAX : (int32_t)dur64;

    /* Base frequency from MIDI table */
    int64_t base_midi = mg_clamp(note->midi_note, 0, 127);
    double base_freq = midi_freq[base_midi];

    /* Apply detune */
    double detuned_freq = base_freq * pow2_cents(chan->detune_cents);

    /* Portamento: glide from previous note's frequency */
    double porta_start_freq = detuned_freq;
    int32_t porta_samples = 0;
    if (chan->portamento_ms > 0 && state->prev_freq > 0.0 &&
        note->beat_pos > state->prev_beat_pos) {
        porta_start_freq = state->prev_freq;
        porta_samples = (int32_t)(chan->portamento_ms * MG_SAMPLE_RATE / 1000);
    }

    /* Update state for next note's portamento */
    state->prev_freq = detuned_freq;
    state->prev_beat_pos = note->beat_pos;

    /* Velocity and volume scaling */
    double vel_scale = (double)mg_clamp(note->velocity, 0, 100) / 100.0;
    double vol_scale = (double)mg_clamp(chan->volume, 0, 100) / 100.0;
    double gain = vel_scale * vol_scale * (double)MG_MAX_AMP / (double)channel_count;

    /* Pan gains (equal-power approximation via linear law) */
    int64_t pan = mg_clamp(chan->pan, -100, 100);
    double left_gain = (double)(100 - pan) / 200.0;
    double right_gain = (double)(100 + pan) / 200.0;

    /* Effect parameters */
    double vib_depth = (double)chan->vibrato_depth;
    double vib_speed_hz = (double)chan->vibrato_speed / 100.0;
    double trem_depth_frac = (double)chan->tremolo_depth / 200.0;
    double trem_speed_hz = (double)chan->tremolo_speed / 100.0;
    double arp_speed_hz = (double)chan->arp_speed / 100.0;
    int arp_enabled = (chan->arp_semi1 != 0 || chan->arp_semi2 != 0);

    /* Noise state (if noise channel) */
    mg_noise_t noise;
    if (chan->waveform == MUSICGEN_WAVE_NOISE) {
        /* Seed noise deterministically per note index */
        uint32_t seed = (uint32_t)(note->beat_pos * 0x9E3779B9u + note->midi_note * 0x517CC1B7u);
        mg_noise_init(&noise, seed);
    }

    /* Waveform phase accumulator */
    double phase = 0.0;
    double vibrato_phase = 0.0;
    double tremolo_phase = 0.0;

    for (int32_t i = start; i < end; i++) {
        int32_t offset = i - start;
        double elapsed_s = (double)offset / (double)MG_SAMPLE_RATE;

        /* --- Frequency chain --- */
        double freq = detuned_freq;

        /* Portamento: lerp from previous note frequency */
        if (porta_samples > 0 && offset < porta_samples) {
            double t = (double)offset / (double)porta_samples;
            freq = porta_start_freq + (detuned_freq - porta_start_freq) * t;
        }

        /* Arpeggio: cycle through [0, semi1, semi2] */
        if (arp_enabled && arp_speed_hz > 0.0) {
            int arp_step = (int)(elapsed_s * arp_speed_hz) % 3;
            int64_t arp_offset = 0;
            if (arp_step == 1)
                arp_offset = chan->arp_semi1;
            else if (arp_step == 2)
                arp_offset = chan->arp_semi2;
            if (arp_offset != 0) {
                int64_t arp_midi = mg_clamp(base_midi + arp_offset, 0, 127);
                double arp_freq = midi_freq[arp_midi];
                /* Apply the ratio of arpeggio freq to base freq */
                freq *= arp_freq / base_freq;
            }
        }

        /* Vibrato: sinusoidal pitch modulation */
        if (vib_depth > 0.0 && vib_speed_hz > 0.0) {
            double vib_val = mg_sin(vibrato_phase);
            freq *= pow2_cents((int64_t)(vib_val * vib_depth));
            vibrato_phase = mg_advance_phase(vibrato_phase, vib_speed_hz / (double)MG_SAMPLE_RATE);
        }

        /* --- Sample generation --- */
        double sample;
        if (chan->waveform == MUSICGEN_WAVE_NOISE) {
            /* Noise channel: MIDI note controls lowpass cutoff */
            double cutoff = midi_freq[mg_clamp(base_midi, 10, 120)];
            sample = mg_noise_sample(&noise, cutoff);
        } else {
            sample = mg_waveform(phase, chan->waveform, chan->duty_cycle);
            phase = mg_advance_phase(phase, freq / (double)MG_SAMPLE_RATE);
        }

        /* ADSR envelope */
        double env = mg_adsr(&chan->envelope, offset, dur);

        /* Tremolo: sinusoidal volume modulation */
        double trem = 1.0;
        if (trem_depth_frac > 0.0 && trem_speed_hz > 0.0) {
            double trem_val = mg_sin(tremolo_phase);
            trem = 1.0 - trem_depth_frac * (1.0 + trem_val);
            if (trem < 0.0)
                trem = 0.0;
            tremolo_phase = mg_advance_phase(tremolo_phase, trem_speed_hz / (double)MG_SAMPLE_RATE);
        }

        /* Final amplitude */
        double amp = sample * env * trem * gain;

        /* Accumulate into stereo buffer */
        int32_t idx = i * 2;
        mg_accum_add_saturated(&accum[idx], (int32_t)(amp * left_gain));
        mg_accum_add_saturated(&accum[idx + 1], (int32_t)(amp * right_gain));
    }
}

//===----------------------------------------------------------------------===//
// Soft Clipping (matches vaud_mixer.c approach)
//===----------------------------------------------------------------------===//

#define MG_CLIP_THRESHOLD 28000

/// @brief Soft-clip a 32-bit accumulator into the 16-bit PCM range with knee compression.
///
/// Beyond `MG_CLIP_THRESHOLD` (~28k of int16's 32767 range) we
/// scale the excess by 1/4 to round off the corner instead of
/// hard-clipping. Saturates at ±32767. Avoids the harshness
/// of digital clipping on summed-note loud passages.
/// @param v Mixed signed accumulator sample.
/// @return Soft-limited signed 16-bit PCM sample.
static int16_t mg_soft_clip(int32_t v) {
    if (v > MG_CLIP_THRESHOLD) {
        v = MG_CLIP_THRESHOLD + (v - MG_CLIP_THRESHOLD) / 4;
        if (v > 32767)
            v = 32767;
    } else if (v < -MG_CLIP_THRESHOLD) {
        v = -MG_CLIP_THRESHOLD + (v + MG_CLIP_THRESHOLD) / 4;
        if (v < -32767)
            v = -32767;
    }
    return (int16_t)v;
}

//===----------------------------------------------------------------------===//
// Public API — Song Builder
//===----------------------------------------------------------------------===//

/// @brief Create a new procedural music song builder at the given BPM (20–300).
/// @details MusicGen builds chiptune-style music programmatically. Add channels
///          with different waveforms, set per-channel effects (vibrato, tremolo,
///          arpeggio, portamento), add notes, then call build() to render to a
///          playable Sound handle.
/// @param bpm Requested tempo, clamped to `[20, 300]`.
/// @return Caller-owned runtime song object, or NULL on allocation failure.
void *rt_musicgen_new(int64_t bpm) {
    mg_song_t *song = (mg_song_t *)rt_obj_new_i64(MG_CLASS_ID, (int64_t)sizeof(mg_song_t));
    if (!song)
        return NULL;

    song->vptr = NULL;
    song->bpm = mg_clamp(bpm, 20, 300);
    song->length_centbeats = 0;
    song->swing = 0;
    song->loopable = 0;
    song->channel_count = 0;
    memset(song->channels, 0, sizeof(song->channels));

    /* Set defaults for all channel slots */
    for (int i = 0; i < MUSICGEN_MAX_CHANNELS; i++) {
        mg_channel_t *ch = &song->channels[i];
        ch->envelope.attack_ms = 10;
        ch->envelope.decay_ms = 50;
        ch->envelope.sustain_pct = 80;
        ch->envelope.release_ms = 100;
        ch->volume = 80;
        ch->duty_cycle = 50;
        ch->pan = 0;
        ch->detune_cents = 0;
        ch->vibrato_depth = 0;
        ch->vibrato_speed = 500; /* 5 Hz */
        ch->tremolo_depth = 0;
        ch->tremolo_speed = 400; /* 4 Hz */
        ch->arp_semi1 = 0;
        ch->arp_semi2 = 0;
        ch->arp_speed = 1500; /* 15 Hz */
        ch->portamento_ms = 0;
    }

    /* No finalizer needed — pure data, no sub-object references */
    return song;
}

/// @brief Add a synthesis channel with the given waveform (0=sine, 1=square, 2=saw, 3=triangle,
/// 4=noise).
/// @param song_ptr Valid MusicGen song object.
/// @param waveform Waveform identifier, clamped to `[0, 4]`.
/// @return Zero-based channel index, or `-1` for an invalid song/full channel table.
int64_t rt_musicgen_add_channel(void *song_ptr, int64_t waveform) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return -1;

    if (song->channel_count < 0 || song->channel_count >= MUSICGEN_MAX_CHANNELS)
        return -1;

    waveform = mg_clamp(waveform, 0, 4);
    int32_t idx = song->channel_count;
    song->channels[idx].waveform = waveform;
    song->channel_count++;

    return (int64_t)idx;
}

//===----------------------------------------------------------------------===//
// Public API — Channel Configuration
//===----------------------------------------------------------------------===//

/// @brief Validate a song handle and retrieve one configured channel.
/// @param song_ptr Opaque MusicGen song object.
/// @param ch Zero-based channel index.
/// @return Mutable channel state, or NULL for an invalid song/index.
static mg_channel_t *mg_get_channel(void *song_ptr, int64_t ch) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return NULL;
    if (song->channel_count < 0 || song->channel_count > MUSICGEN_MAX_CHANNELS || ch < 0 ||
        ch >= (int64_t)song->channel_count)
        return NULL;
    return &song->channels[ch];
}

/// @brief Configure the ADSR envelope (Attack, Decay, Sustain level, Release) for a track.
///
/// Times are in milliseconds; sustain is a 0.0–1.0 amplitude scale.
/// Applied to every note added to the track until changed again.
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param attack_ms Attack duration, clamped to `[0, 5000]`.
/// @param decay_ms Decay duration, clamped to `[0, 5000]`.
/// @param sustain_pct Sustain level, clamped to `[0, 100]`.
/// @param release_ms Release duration, clamped to `[0, 5000]`.
void rt_musicgen_set_envelope(void *song,
                              int64_t ch,
                              int64_t attack_ms,
                              int64_t decay_ms,
                              int64_t sustain_pct,
                              int64_t release_ms) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->envelope.attack_ms = mg_clamp(attack_ms, 0, 5000);
    c->envelope.decay_ms = mg_clamp(decay_ms, 0, 5000);
    c->envelope.sustain_pct = mg_clamp(sustain_pct, 0, 100);
    c->envelope.release_ms = mg_clamp(release_ms, 0, 5000);
}

/// @brief Set the volume of a channel (0–100).
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param volume Requested channel percentage, clamped to `[0, 100]`.
void rt_musicgen_set_channel_vol(void *song, int64_t ch, int64_t volume) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->volume = mg_clamp(volume, 0, 100);
}

/// @brief Set the duty cycle for square wave channels (1–99, default 50).
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param duty Requested high-phase percentage, clamped to `[1, 99]`.
void rt_musicgen_set_duty(void *song, int64_t ch, int64_t duty) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->duty_cycle = mg_clamp(duty, 1, 99);
}

/// @brief Set the stereo pan for a channel (-100=left, 0=center, 100=right).
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param pan Requested pan position, clamped to `[-100, 100]`.
void rt_musicgen_set_pan(void *song, int64_t ch, int64_t pan) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->pan = mg_clamp(pan, -100, 100);
}

/// @brief Detune a channel by the given number of cents (-1200 to +1200).
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param cents Pitch offset, clamped to `[-1200, 1200]`.
void rt_musicgen_set_detune(void *song, int64_t ch, int64_t cents) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->detune_cents = mg_clamp(cents, -1200, 1200);
}

/// @brief Set vibrato (pitch wobble) depth and speed for a channel.
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param depth Pitch excursion in cents, clamped to `[0, 200]`.
/// @param speed Hundredths of a hertz, clamped to `[0, 5000]`.
void rt_musicgen_set_vibrato(void *song, int64_t ch, int64_t depth, int64_t speed) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->vibrato_depth = mg_clamp(depth, 0, 200);
    c->vibrato_speed = mg_clamp(speed, 0, 5000);
}

/// @brief Set tremolo (volume wobble) depth and speed for a channel.
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param depth Modulation depth percentage, clamped to `[0, 100]`.
/// @param speed Hundredths of a hertz, clamped to `[0, 5000]`.
void rt_musicgen_set_tremolo(void *song, int64_t ch, int64_t depth, int64_t speed) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->tremolo_depth = mg_clamp(depth, 0, 100);
    c->tremolo_speed = mg_clamp(speed, 0, 5000);
}

/// @brief Set arpeggio (rapid pitch cycling) intervals and speed for a channel.
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param semi1 First pitch interval, clamped to `[0, 24]` semitones.
/// @param semi2 Second pitch interval, clamped to `[0, 24]` semitones.
/// @param speed Cycling rate in hundredths of a hertz, clamped to `[0, 5000]`.
void rt_musicgen_set_arpeggio(void *song, int64_t ch, int64_t semi1, int64_t semi2, int64_t speed) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->arp_semi1 = mg_clamp(semi1, 0, 24);
    c->arp_semi2 = mg_clamp(semi2, 0, 24);
    c->arp_speed = mg_clamp(speed, 0, 5000);
}

/// @brief Set portamento (pitch slide) speed for a channel (0 = off, ms to reach new pitch).
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param speed_ms Slide duration, clamped to `[0, 2000]` milliseconds.
void rt_musicgen_set_portamento(void *song, int64_t ch, int64_t speed_ms) {
    mg_channel_t *c = mg_get_channel(song, ch);
    if (!c)
        return;
    c->portamento_ms = mg_clamp(speed_ms, 0, 2000);
}

//===----------------------------------------------------------------------===//
// Public API — Notes
//===----------------------------------------------------------------------===//

/// @brief Schedule a note on a track. Equivalent to `add_note_vel` with full velocity.
/// @param song Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param beat_pos Start position in centbeats.
/// @param midi_note MIDI note number, clamped to `[0, 127]`.
/// @param duration Positive duration in centbeats.
/// @return `1` on success or `0` when the note cannot be scheduled.
int64_t rt_musicgen_add_note(
    void *song, int64_t ch, int64_t beat_pos, int64_t midi_note, int64_t duration) {
    return rt_musicgen_add_note_vel(song, ch, beat_pos, midi_note, duration, 100);
}

/// @brief Schedule a note with explicit velocity (0-100) on a track.
///
/// `beat_pos` and `duration` are centbeats (100 = one beat), and
/// `midi_note` is the MIDI note number (0-127, 60 = middle C).
/// Velocity scales note volume linearly. Returns 1 on success or 0 on failure.
/// Notes at or beyond the maximum renderable span are rejected so clamped
/// extreme timestamps cannot create zero-length notes at the end boundary.
/// @param song_ptr Valid MusicGen song object.
/// @param ch Zero-based channel index.
/// @param beat_pos Start position in centbeats, normalized to the render span.
/// @param midi_note MIDI note number, clamped to `[0, 127]`.
/// @param duration Duration in centbeats, clipped to the remaining render span.
/// @param velocity Note gain percentage, clamped to `[0, 100]`.
/// @return `1` on success or `0` for invalid input/full note storage.
int64_t rt_musicgen_add_note_vel(void *song_ptr,
                                 int64_t ch,
                                 int64_t beat_pos,
                                 int64_t midi_note,
                                 int64_t duration,
                                 int64_t velocity) {
    mg_channel_t *c = mg_get_channel(song_ptr, ch);
    if (!c)
        return 0;

    if (c->note_count < 0 || c->note_count >= MUSICGEN_MAX_NOTES)
        return 0;

    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return 0;
    int64_t max_centbeats = mg_max_centbeats_for_bpm(song->bpm);
    if (beat_pos < 0)
        beat_pos = 0;
    if (beat_pos > max_centbeats)
        beat_pos = max_centbeats;
    if (beat_pos >= max_centbeats)
        return 0;
    if (duration < 1)
        duration = 1;
    if (duration > max_centbeats - beat_pos)
        duration = max_centbeats - beat_pos;

    mg_note_t *note = &c->notes[c->note_count];
    note->beat_pos = beat_pos;
    note->midi_note = mg_clamp(midi_note, 0, 127);
    note->duration = duration;
    note->velocity = mg_clamp(velocity, 0, 100);
    note->order = c->note_count;

    c->note_count++;
    return 1;
}

//===----------------------------------------------------------------------===//
// Public API — Song Properties
//===----------------------------------------------------------------------===//

/// @brief Set the total song length in centbeats (100 centbeats = 1 beat).
/// @param song_ptr Valid MusicGen song object.
/// @param length_centbeats Requested length, clamped to the five-minute render cap.
void rt_musicgen_set_length(void *song_ptr, int64_t length_centbeats) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return;
    song->length_centbeats = mg_clamp(length_centbeats, 0, mg_max_centbeats_for_bpm(song->bpm));
}

/// @brief Set the swing amount (0–100; offbeat notes shifted later for groove feel).
/// @param song_ptr Valid MusicGen song object.
/// @param swing Requested off-beat delay percentage, clamped to `[0, 100]`.
void rt_musicgen_set_swing(void *song_ptr, int64_t swing) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return;
    song->swing = mg_clamp(swing, 0, 100);
}

/// @brief Mark the song as loopable (seamless loop point at the end).
/// @param song_ptr Valid MusicGen song object.
/// @param loopable Non-zero to apply loop-boundary treatment during rendering.
void rt_musicgen_set_loopable(void *song_ptr, int64_t loopable) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return;
    song->loopable = (loopable != 0) ? 1 : 0;
}

/// @brief Get the song's beats-per-minute.
/// @param song_ptr MusicGen song object.
/// @return Stored tempo, or zero for an invalid object.
int64_t rt_musicgen_get_bpm(void *song_ptr) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return 0;
    return mg_clamp(song->bpm, 20, 300);
}

/// @brief Get the song length in centbeats.
/// @param song_ptr MusicGen song object.
/// @return Stored length, or zero for an invalid object.
int64_t rt_musicgen_get_length(void *song_ptr) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return 0;
    return mg_clamp(song->length_centbeats, 0, mg_max_centbeats_for_bpm(song->bpm));
}

/// @brief Get the number of channels added to the song.
/// @param song_ptr MusicGen song object.
/// @return Active channel count, or zero for an invalid object.
int64_t rt_musicgen_get_channel_count(void *song_ptr) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return 0;
    return mg_clamp(song->channel_count, 0, MUSICGEN_MAX_CHANNELS);
}

/// @brief Validate retained channel configuration before rendering arithmetic.
/// @param channel Channel storage to inspect.
/// @return Non-zero when counts and every numeric setting are in public API ranges.
static int mg_channel_renderable(const mg_channel_t *channel) {
    return channel && channel->note_count >= 0 && channel->note_count <= MUSICGEN_MAX_NOTES &&
           channel->waveform >= MUSICGEN_WAVE_SINE && channel->waveform <= MUSICGEN_WAVE_NOISE &&
           channel->envelope.attack_ms >= 0 && channel->envelope.attack_ms <= 5000 &&
           channel->envelope.decay_ms >= 0 && channel->envelope.decay_ms <= 5000 &&
           channel->envelope.sustain_pct >= 0 && channel->envelope.sustain_pct <= 100 &&
           channel->envelope.release_ms >= 0 && channel->envelope.release_ms <= 5000 &&
           channel->volume >= 0 && channel->volume <= 100 && channel->duty_cycle >= 1 &&
           channel->duty_cycle <= 99 && channel->pan >= -100 && channel->pan <= 100 &&
           channel->detune_cents >= -1200 && channel->detune_cents <= 1200 &&
           channel->vibrato_depth >= 0 && channel->vibrato_depth <= 200 &&
           channel->vibrato_speed >= 0 && channel->vibrato_speed <= 5000 &&
           channel->tremolo_depth >= 0 && channel->tremolo_depth <= 100 &&
           channel->tremolo_speed >= 0 && channel->tremolo_speed <= 5000 &&
           channel->arp_semi1 >= 0 && channel->arp_semi1 <= 24 && channel->arp_semi2 >= 0 &&
           channel->arp_semi2 <= 24 && channel->arp_speed >= 0 && channel->arp_speed <= 5000 &&
           channel->portamento_ms >= 0 && channel->portamento_ms <= 2000;
}

//===----------------------------------------------------------------------===//
// Public API — Build (Pre-render to Sound)
//===----------------------------------------------------------------------===//

/// @brief Render the song to PCM audio and return a playable Sound handle.
/// @details Mixes all channels into a stereo 44100 Hz WAV buffer. Each note is
///          synthesized with its channel's waveform, ADSR envelope, and effects
///          (vibrato, tremolo, arpeggio, portamento). The result can be played
///          with rt_sound_play or loaded as music.
/// @param song_ptr Valid MusicGen song with positive length and at least one channel.
/// @return Caller-owned runtime Sound handle, or NULL when audio is unavailable,
///         the song is invalid/empty, a size limit is exceeded, or allocation/loading fails.
void *rt_musicgen_build(void *song_ptr) {
    mg_song_t *song = mg_as_song(song_ptr);
    if (!song)
        return NULL;
    if (!rt_audio_is_available())
        return NULL;

    if (song->bpm < 20 || song->bpm > 300 || song->channel_count <= 0 ||
        song->channel_count > MUSICGEN_MAX_CHANNELS || song->length_centbeats <= 0 ||
        song->length_centbeats > mg_max_centbeats_for_bpm(song->bpm) || song->swing < 0 ||
        song->swing > 100)
        return NULL;

    int32_t active_channel_count = 0;
    for (int32_t ch = 0; ch < song->channel_count; ++ch) {
        mg_channel_t *channel = &song->channels[ch];
        if (!mg_channel_renderable(channel))
            return NULL;
        if (channel->note_count > 0 && channel->volume > 0)
            ++active_channel_count;
    }
    if (active_channel_count == 0)
        return NULL;

    /* Calculate total frames */
    int32_t samples_per_beat = (int32_t)((int64_t)MG_SAMPLE_RATE * 60 / song->bpm);
    int64_t total_frames_64 = 0;
    if (!mg_centbeats_to_frames(song->length_centbeats, samples_per_beat, &total_frames_64))
        return NULL;

    /* Cap at 5 minutes */
    int64_t max_frames = (int64_t)MG_MAX_DURATION_S * MG_SAMPLE_RATE;
    if (total_frames_64 > max_frames)
        total_frames_64 = max_frames;
    if (total_frames_64 <= 0)
        return NULL;

    int32_t total_frames = (int32_t)total_frames_64;

    /* Allocate 32-bit stereo accumulator (zeroed) */
    if ((size_t)total_frames > SIZE_MAX / 2 / sizeof(int32_t))
        return NULL;
    size_t accum_size = (size_t)total_frames * 2 * sizeof(int32_t);
    int32_t *accum = (int32_t *)calloc(1, accum_size);
    if (!accum)
        return NULL;

    /* Render all channels and notes */
    for (int32_t ch = 0; ch < song->channel_count; ch++) {
        mg_channel_t *chan = &song->channels[ch];
        if (chan->note_count <= 0 || chan->volume <= 0)
            continue;
        mg_note_t *notes = chan->notes;
        mg_note_t *sorted_notes = NULL;
        if (chan->note_count > 1 && chan->portamento_ms > 0) {
            if ((size_t)chan->note_count > SIZE_MAX / sizeof(mg_note_t)) {
                free(accum);
                return NULL;
            }
            sorted_notes = (mg_note_t *)malloc((size_t)chan->note_count * sizeof(mg_note_t));
            if (!sorted_notes) {
                free(accum);
                return NULL;
            }
            memcpy(sorted_notes, chan->notes, (size_t)chan->note_count * sizeof(mg_note_t));
            qsort(sorted_notes, (size_t)chan->note_count, sizeof(mg_note_t), mg_note_compare);
            notes = sorted_notes;
        }

        mg_render_state_t state;
        state.prev_freq = 0.0;
        state.prev_beat_pos = -1;

        for (int32_t n = 0; n < chan->note_count; n++) {
            mg_render_note(accum,
                           total_frames,
                           &notes[n],
                           chan,
                           samples_per_beat,
                           song->swing,
                           &state,
                           active_channel_count);
        }
        free(sorted_notes);
    }

    /* Soft-clip to 16-bit stereo */
    if ((size_t)total_frames > SIZE_MAX / 2u) {
        free(accum);
        return NULL;
    }
    size_t pcm_count = (size_t)total_frames * 2;
    if (pcm_count > SIZE_MAX / sizeof(int16_t)) {
        free(accum);
        return NULL;
    }
    int16_t *pcm = (int16_t *)malloc(pcm_count * sizeof(int16_t));
    if (!pcm) {
        free(accum);
        return NULL;
    }

    for (size_t i = 0; i < pcm_count; i++)
        pcm[i] = mg_soft_clip(accum[i]);

    free(accum);

    /* Loop crossfade: blend the end into the start for seamless looping */
    if (song->loopable && total_frames > 0) {
        int32_t fade_frames = MG_CROSSFADE_MS * MG_SAMPLE_RATE / 1000;
        /* Don't exceed 1/4 of the song */
        if (fade_frames > total_frames / 4)
            fade_frames = total_frames / 4;

        if (fade_frames > 0) {
            for (int32_t i = 0; i < fade_frames; i++) {
                double t = (double)i / (double)fade_frames;
                int32_t end_idx = (total_frames - fade_frames + i) * 2;
                int32_t start_idx = i * 2;

                /* Blend: start = start * t + end * (1 - t) */
                for (int c = 0; c < 2; c++) {
                    double s = (double)pcm[start_idx + c];
                    double e = (double)pcm[end_idx + c];
                    pcm[start_idx + c] = (int16_t)(s * t + e * (1.0 - t));
                }
            }

            /* Keep the requested length; loop timing must not drift. */
        }
    }

    /* Build stereo WAV and create Sound object */
    uint32_t data_size = 0;
    size_t wav_size = 0;
    if (!mg_wav_sizes(total_frames, &data_size, &wav_size)) {
        free(pcm);
        return NULL;
    }

    uint8_t *wav_buf = (uint8_t *)malloc(wav_size);
    if (!wav_buf) {
        free(pcm);
        return NULL;
    }

    mg_write_wav_header(wav_buf, data_size);
    for (size_t i = 0; i < pcm_count; ++i) {
        uint16_t sample = (uint16_t)pcm[i];
        wav_buf[MG_WAV_HEADER + i * 2] = (uint8_t)sample;
        wav_buf[MG_WAV_HEADER + i * 2 + 1] = (uint8_t)(sample >> 8);
    }
    free(pcm);

    void *sound = rt_sound_load_mem(wav_buf, (int64_t)wav_size);
    free(wav_buf);

    return sound;
}
