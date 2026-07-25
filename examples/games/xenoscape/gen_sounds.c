//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: examples/games/xenoscape/gen_sounds.c
// Purpose: Procedural WAV sound-effect generator for the Xenoscape demo assets.
// Key invariants:
//   - Standalone translation unit; no cross-layer dependencies.
// Ownership/Lifetime:
//   - No long-lived state; all allocations are scoped to the run.
// Links: docs/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file gen_sounds.c
 * @brief Generates the PCM WAV sound effects used by the Xenoscape example.
 *
 * @details
 * This standalone utility synthesizes twelve deterministic, monophonic sound
 * effects at 22,050 samples per second. Each generator combines elementary
 * waveforms with an attack-decay-sustain-release envelope, converts the result
 * to signed 16-bit PCM, and writes a RIFF/WAVE file below `sounds/`.
 *
 * The utility intentionally uses only the C standard library and the platform
 * directory-creation API so that assets can be regenerated without Zanna
 * compiler or runtime dependencies.
 */

// gen_sounds.c -- Procedural WAV sound effect generator for sidescroller demo
// Compile: cc -o gen_sounds gen_sounds.c -lm
// Run:     ./gen_sounds
// Creates .wav files in a sounds/ subdirectory next to this file.

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include(<direct.h>)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#define SAMPLE_RATE 22050
#define PI 3.14159265358979323846

/**
 * @brief Create the sound output directory through the available host API.
 *
 * @param path Null-terminated directory path.
 *
 * @return Zero on success and a nonzero value on failure.
 */
static int create_output_directory(const char *path) {
#if __has_include(<direct.h>)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/**
 * @brief Write signed samples as a mono, 16-bit PCM RIFF/WAVE file.
 *
 * The routine emits a canonical 44-byte WAV header followed by `count` samples
 * in host byte order. The generated assets therefore assume the little-endian
 * hosts on which the Xenoscape examples are prepared.
 *
 * @param path Null-terminated destination path opened in binary write mode.
 * @param samples Contiguous input array containing at least `count` samples.
 * @param count Number of samples to encode.
 *
 * @pre `path` and `samples` are non-null.
 * @pre `count` is non-negative and small enough that `36 + count * 2` fits in
 *      both `int` and the 32-bit RIFF chunk-size fields.
 *
 * @note Failure to open the file is reported to standard error. Individual
 *       write and close errors are not checked by this example utility.
 */
static void write_wav(const char *path, const int16_t *samples, int count) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return;
    }

    int data_size = count * 2;
    int file_size = 36 + data_size;

    // RIFF header
    fwrite("RIFF", 1, 4, f);
    uint32_t v = (uint32_t)file_size;
    fwrite(&v, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    v = 16;
    fwrite(&v, 4, 1, f); // chunk size
    uint16_t s = 1;
    fwrite(&s, 2, 1, f); // PCM format
    s = 1;
    fwrite(&s, 2, 1, f); // mono
    v = SAMPLE_RATE;
    fwrite(&v, 4, 1, f); // sample rate
    v = SAMPLE_RATE * 2;
    fwrite(&v, 4, 1, f); // byte rate
    s = 2;
    fwrite(&s, 2, 1, f); // block align
    s = 16;
    fwrite(&s, 2, 1, f); // bits per sample

    // data chunk
    fwrite("data", 1, 4, f);
    v = (uint32_t)data_size;
    fwrite(&v, 4, 1, f);
    fwrite(samples, 2, (size_t)count, f);

    fclose(f);
    printf("  wrote %s (%d samples, %.2fs)\n", path, count, (double)count / SAMPLE_RATE);
}

/**
 * @brief Saturate a normalized sample to the closed interval [-1, 1].
 *
 * @param x Sample amplitude to constrain.
 * @return `x` when it is in range, otherwise the nearest interval endpoint.
 *
 * @note A NaN input is returned unchanged because both comparisons are false.
 */
static double clamp(double x) {
    return x < -1.0 ? -1.0 : x > 1.0 ? 1.0 : x;
}

/**
 * @brief Evaluate an attack-decay-sustain-release amplitude envelope.
 *
 * Attack rises linearly from zero to one, decay falls to `sus`, sustain holds
 * that level, and release falls linearly to zero at `dur`. All time-valued
 * arguments use seconds.
 *
 * @param t Time at which to sample the envelope.
 * @param dur Total envelope duration.
 * @param atk Attack-segment duration.
 * @param dec Decay-segment duration.
 * @param sus Sustain amplitude, conventionally in the range [0, 1].
 * @param rel Release-segment duration.
 * @return The piecewise-linear amplitude at `t`, or zero at and after `dur`.
 *
 * @pre `atk`, `dec`, and `rel` are positive.
 * @pre `dur` is at least `atk + dec + rel`.
 */
static double envelope(double t, double dur, double atk, double dec, double sus, double rel) {
    double rel_start = dur - rel;
    if (t < atk)
        return t / atk;
    if (t < atk + dec)
        return 1.0 - (1.0 - sus) * (t - atk) / dec;
    if (t < rel_start)
        return sus;
    if (t < dur)
        return sus * (1.0 - (t - rel_start) / rel);
    return 0.0;
}

/**
 * @brief Evaluate a unit-amplitude sine oscillator.
 *
 * @param phase Oscillator phase measured in cycles, not radians.
 * @return `sin(2 * pi * phase)`, in the interval [-1, 1].
 */
static double osc_sin(double phase) {
    return sin(phase * 2.0 * PI);
}

/**
 * @brief Evaluate a bipolar square-wave oscillator.
 *
 * @param phase Oscillator phase measured in cycles.
 * @return Positive one during the first half-cycle and negative one during the
 *         second half-cycle.
 *
 * @note Callers in this utility pass non-negative phases. Negative phases
 *       inherit the signed-remainder behavior of `fmod`.
 */
static double osc_square(double phase) {
    double p = fmod(phase, 1.0);
    return p < 0.5 ? 1.0 : -1.0;
}

/**
 * @brief Produce one pseudo-random normalized noise sample.
 *
 * @return A value derived from `rand()` and linearly mapped to [-1, 1].
 *
 * @note The sequence is deterministic after `main()` seeds the process-global
 *       C pseudo-random generator with 42.
 */
static double osc_noise(void) {
    return (double)rand() / RAND_MAX * 2.0 - 1.0;
}

/**
 * @brief Evaluate a bipolar triangle-wave oscillator.
 *
 * @param phase Oscillator phase measured in cycles.
 * @return A piecewise-linear waveform that rises from zero to one, falls
 *         through zero to negative one, and returns to zero each cycle.
 *
 * @note The waveform contract assumes a non-negative phase.
 */
static double osc_tri(double phase) {
    double p = fmod(phase, 1.0);
    if (p < 0.25)
        return p * 4.0;
    if (p < 0.75)
        return 2.0 - p * 4.0;
    return p * 4.0 - 4.0;
}

// ============================================================================

/**
 * @brief Generate the short rising square-wave jump effect.
 *
 * The 0.15-second chirp sweeps from 300 Hz to 900 Hz beneath a fast ADSR
 * envelope and writes the result to `path`.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_jump(const char *path) {
    // Rising square wave chirp
    double dur = 0.15;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double freq = 300.0 + 600.0 * (t / dur); // 300 -> 900 Hz
        double phase = t * freq;
        double env = envelope(t, dur, 0.005, 0.02, 0.6, 0.05);
        buf[i] = (int16_t)(clamp(osc_square(phase) * env * 0.4) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the descending tonal-and-noise shooting effect.
 *
 * The 0.12-second effect sweeps a sine component from 1,200 Hz to 400 Hz and
 * mixes in low-level pseudo-random noise before envelope shaping.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_shoot(const char *path) {
    // Short descending noise + sine zap
    double dur = 0.12;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double freq = 1200.0 - 800.0 * (t / dur);
        double phase = t * freq;
        double env = envelope(t, dur, 0.002, 0.03, 0.3, 0.04);
        double sig = osc_sin(phase) * 0.5 + osc_noise() * 0.2;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the two-note coin collection chime.
 *
 * The effect plays an approximately B5 tone followed by E6, giving each
 * 0.1-second segment an independent envelope.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_coin(const char *path) {
    // Two-tone chime (classic coin sound)
    double dur = 0.2;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double sig = 0.0;
        if (t < 0.1) {
            double env = envelope(t, 0.1, 0.002, 0.02, 0.5, 0.03);
            sig = osc_sin(t * 988.0) * env; // B5
        } else {
            double t2 = t - 0.1;
            double env = envelope(t2, 0.1, 0.002, 0.02, 0.4, 0.05);
            sig = osc_sin(t * 1319.0) * env; // E6
        }
        buf[i] = (int16_t)(clamp(sig * 0.5) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the low descending hurt effect.
 *
 * A sine sweep from 120 Hz to 60 Hz is mixed with noise and attenuated over
 * 0.25 seconds to produce a short impact sound.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_hurt(const char *path) {
    // Low thud with noise
    double dur = 0.25;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double freq = 120.0 - 60.0 * (t / dur);
        double phase = t * freq;
        double env = envelope(t, dur, 0.005, 0.05, 0.3, 0.1);
        double sig = osc_sin(phase) * 0.5 + osc_noise() * 0.3;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the enemy-death pop effect.
 *
 * The effect combines a descending 400-to-100 Hz square wave with a stronger
 * noise component under a rapid 0.18-second envelope.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_enemy_death(const char *path) {
    // Quick pop/splat
    double dur = 0.18;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double freq = 400.0 - 300.0 * (t / dur);
        double phase = t * freq;
        double env = envelope(t, dur, 0.003, 0.03, 0.2, 0.08);
        double sig = osc_square(phase) * 0.3 + osc_noise() * 0.4;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the ascending three-note power-up arpeggio.
 *
 * The C5, E5, and G5 segments combine a fundamental sine with a quieter
 * triangle harmonic and occupy equal thirds of the 0.35-second effect.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_powerup(const char *path) {
    // Ascending arpeggio — three quick notes
    double dur = 0.35;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    double notes[] = {523.0, 659.0, 784.0}; // C5, E5, G5
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        int note_idx = (int)(t / (dur / 3.0));
        if (note_idx > 2)
            note_idx = 2;
        double note_t = t - note_idx * (dur / 3.0);
        double note_dur = dur / 3.0;
        double env = envelope(note_t, note_dur, 0.005, 0.02, 0.5, 0.03);
        double sig = osc_sin(t * notes[note_idx]) * 0.4 + osc_tri(t * notes[note_idx] * 2.0) * 0.15;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the two-note checkpoint confirmation chime.
 *
 * The effect plays A5 for 0.12 seconds and D6 for the remaining 0.18 seconds,
 * with separate envelopes that create a clear transition.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_checkpoint(const char *path) {
    // Cheerful two-note ding
    double dur = 0.3;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double sig = 0.0;
        if (t < 0.12) {
            double env = envelope(t, 0.12, 0.003, 0.02, 0.5, 0.03);
            sig = osc_sin(t * 880.0) * env; // A5
        } else {
            double t2 = t - 0.12;
            double env = envelope(t2, 0.18, 0.003, 0.03, 0.4, 0.08);
            sig = osc_sin(t * 1175.0) * env; // D6
        }
        buf[i] = (int16_t)(clamp(sig * 0.5) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the four-note level-completion fanfare.
 *
 * Four 0.2-second C5, E5, G5, and C6 segments combine a sine fundamental with
 * second and third harmonics to form the longest generated effect.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_level_complete(const char *path) {
    // Victory fanfare: C-E-G-C ascending with harmonics
    double dur = 0.8;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    double notes[] = {523.0, 659.0, 784.0, 1047.0}; // C5-E5-G5-C6
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        int note_idx = (int)(t / 0.2);
        if (note_idx > 3)
            note_idx = 3;
        double note_t = t - note_idx * 0.2;
        double note_dur = (note_idx == 3) ? 0.2 : 0.2;
        double env = envelope(note_t, note_dur, 0.005, 0.03, 0.6, 0.05);
        double freq = notes[note_idx];
        double sig = osc_sin(t * freq) * 0.35 + osc_sin(t * freq * 2.0) * 0.1 +
                     osc_tri(t * freq * 3.0) * 0.05;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the short square-wave menu-selection blip.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 *
 * @details The effect is a 660 Hz square wave shaped to 0.06 seconds.
 */
static void gen_menu_select(const char *path) {
    // Quick blip
    double dur = 0.06;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double env = envelope(t, dur, 0.002, 0.01, 0.5, 0.02);
        double sig = osc_square(t * 660.0) * 0.3;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the descending player-death effect.
 *
 * A 500-to-100 Hz square-wave sweep, a subharmonic sine, and a small noise
 * component are blended beneath a 0.6-second envelope.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_death(const char *path) {
    // Descending tone with noise
    double dur = 0.6;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double freq = 500.0 - 400.0 * (t / dur);
        double phase = t * freq;
        double env = envelope(t, dur, 0.01, 0.05, 0.5, 0.2);
        double sig = osc_square(phase) * 0.3 + osc_sin(phase * 0.5) * 0.2 + osc_noise() * 0.1;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the short rising stomp effect.
 *
 * The 0.1-second sound combines a 600-to-800 Hz sine chirp with a quieter
 * square-wave subharmonic to create a percussive pop.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 */
static void gen_stomp(const char *path) {
    // Bouncy pop for stomping enemies
    double dur = 0.1;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double freq = 600.0 + 200.0 * (t / dur);
        double phase = t * freq;
        double env = envelope(t, dur, 0.002, 0.02, 0.4, 0.03);
        double sig = osc_sin(phase) * 0.5 + osc_square(phase * 0.5) * 0.15;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Generate the subtle menu-navigation tick.
 *
 * @param path Destination WAV path.
 * @pre `path` is non-null and the parent directory exists.
 * @pre Allocation of the fixed-size sample buffer succeeds.
 *
 * @details The effect is a lightly amplified 440 Hz sine lasting 0.04 seconds.
 */
static void gen_menu_move(const char *path) {
    // Subtle tick for menu navigation
    double dur = 0.04;
    int n = (int)(dur * SAMPLE_RATE);
    int16_t *buf = calloc((size_t)n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double env = envelope(t, dur, 0.001, 0.01, 0.3, 0.01);
        double sig = osc_sin(t * 440.0) * 0.3;
        buf[i] = (int16_t)(clamp(sig * env) * 32000);
    }
    write_wav(path, buf, n);
    free(buf);
}

/**
 * @brief Create the output directory and regenerate all Xenoscape sound files.
 *
 * The C pseudo-random generator is seeded with a fixed value before any noise
 * oscillator is sampled, making repeated invocations deterministic for a given
 * C library implementation. Existing files with matching names are replaced.
 *
 * @return Always zero after attempting all twelve file generations.
 *
 * @note Directory-creation failures and per-file write failures are reported
 *       only indirectly; generation continues so one failure does not suppress
 *       attempts for the remaining assets.
 */
int main(void) {
    srand(42); // deterministic noise

    const char *dir = "sounds";
    if (create_output_directory(dir) != 0 && errno != EEXIST) {
        perror("Cannot create sounds");
        return 1;
    }

    printf("Generating sidescroller sound effects...\n");
    gen_jump("sounds/jump.wav");
    gen_shoot("sounds/shoot.wav");
    gen_coin("sounds/coin.wav");
    gen_hurt("sounds/hurt.wav");
    gen_enemy_death("sounds/enemy_death.wav");
    gen_powerup("sounds/powerup.wav");
    gen_checkpoint("sounds/checkpoint.wav");
    gen_level_complete("sounds/level_complete.wav");
    gen_menu_select("sounds/menu_select.wav");
    gen_menu_move("sounds/menu_move.wav");
    gen_death("sounds/death.wav");
    gen_stomp("sounds/stomp.wav");
    printf("Done! 12 sound effects generated.\n");

    return 0;
}
