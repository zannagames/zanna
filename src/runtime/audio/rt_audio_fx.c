//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_audio_fx.c
// Purpose: Implements real-time-safe mix-group insert effects for audio buses.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements per-mix-group insert effects and in-place DSP processing.
/// @details Effect chains are process-global linked lists protected by a small
///          spinlock. Control-thread mutations wait for the lock, while the
///          mixer-facing query and processing entry points use a non-blocking
///          try-lock and leave audio untouched under contention. Filters,
///          delays, and reverbs maintain independent stereo state and sanitize
///          non-finite/denormal feedback values to keep a damaged effect from
///          poisoning the rest of a bus.

#include "rt_audio_fx.h"

#include "rt_mixgroup.h"
#include "rt_platform.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if RT_COMPILER_MSVC
#include <intrin.h>
#endif

#define RT_AUDIO_FX_DEFAULT_RATE 44100
#define RT_AUDIO_FX_MAX_DELAY_SECONDS 2.0
#define RT_AUDIO_FX_PI 3.14159265358979323846

/// @brief Runtime discriminator for the DSP state stored in an effect node.
typedef enum {
    RT_AUDIO_FX_BIQUAD = 0,
    RT_AUDIO_FX_DELAY = 1,
    RT_AUDIO_FX_REVERB = 2,
} rt_audio_fx_kind;

/// @brief Coefficients and transposed direct-form state for one stereo biquad.
typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1_l;
    float z2_l;
    float z1_r;
    float z2_r;
} rt_biquad_state;

/// @brief Circular stereo delay-line state for a feedback delay insert.
typedef struct {
    float *buffer;
    int32_t frames;
    int32_t pos;
    float feedback;
    float wet;
    float dry;
} rt_delay_state;

/// @brief One circular comb or all-pass line used by the reverb network.
typedef struct {
    float *buffer;
    int32_t frames;
    int32_t pos;
    float filter;
} rt_reverb_line;

/// @brief Parallel-comb/serial-all-pass state for a stereo reverb insert.
typedef struct {
    rt_reverb_line comb_l[8];
    rt_reverb_line comb_r[8];
    rt_reverb_line allpass_l[4];
    rt_reverb_line allpass_r[4];
    float room;
    float damp;
    float wet;
    float dry;
} rt_reverb_state;

/// @brief One linked effect-chain node owned by a mix group.
typedef struct rt_audio_fx {
    int64_t id;
    rt_audio_fx_kind kind;
    int8_t bypass;

    union {
        rt_biquad_state biquad;
        rt_delay_state delay;
        rt_reverb_state reverb;
    } u;
    struct rt_audio_fx *next;
} rt_audio_fx;

static rt_audio_fx *g_group_fx[RT_MIXGROUP_MAX_GROUPS] = {0};
static int64_t g_next_fx_id = 1;
static volatile int g_fx_lock = 0;

/// @brief Attempt to acquire the effect registry without waiting.
/// @details Used by real-time mixer callbacks so control-thread mutations never
///          block audio rendering.
/// @return Non-zero when the caller acquired the lock; zero on contention.
static int fx_try_lock(void) {
#if RT_COMPILER_MSVC
    return _InterlockedExchange8((volatile char *)&g_fx_lock, 1) == 0;
#else
    return !__atomic_test_and_set(&g_fx_lock, __ATOMIC_ACQUIRE);
#endif
}

/// @brief Acquire the effect registry lock, spinning until it becomes available.
/// @details Intended for control-thread mutations and never for the
///          latency-sensitive mixer callback.
static void fx_lock(void) {
    while (!fx_try_lock()) {
    }
}

/// @brief Release the effect registry lock with release ordering.
static void fx_unlock(void) {
#if RT_COMPILER_MSVC
    _InterlockedExchange8((volatile char *)&g_fx_lock, 0);
#else
    __atomic_clear(&g_fx_lock, __ATOMIC_RELEASE);
#endif
}

/// @brief Check whether an identifier can index the fixed group-chain table.
/// @details This validates only numeric range, not whether the higher-level
///          named-group registry currently marks the slot as in use.
/// @param group Mix-group identifier to validate.
/// @return Non-zero for values in `[0, RT_MIXGROUP_MAX_GROUPS)`.
static int group_valid(int64_t group) {
    return group >= 0 && group < RT_MIXGROUP_MAX_GROUPS;
}

/// @brief Clamp a floating-point value to an inclusive range.
/// @details Non-finite input maps to the lower bound.
/// @param value Value to normalize.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return Normalized value in `[@p lo, @p hi]`.
static float clampf_range(float value, float lo, float hi) {
    if (!isfinite(value))
        return lo;
    if (value < lo)
        return lo;
    if (value > hi)
        return hi;
    return value;
}

/// @brief Prevent non-finite or denormal-scale DSP feedback from propagating.
/// @param value Intermediate sample or filter-state value.
/// @return Zero for non-finite values or magnitudes below `1e-20`; otherwise
///         the original value.
static float sanitize_feedback(float value) {
    if (fabsf(value) < 1.0e-20f)
        return 0.0f;
    if (!isfinite(value))
        return 0.0f;
    return value;
}

/// @brief Convert a delay duration to frames at the fixed DSP design rate.
/// @details Invalid/non-positive input becomes one millisecond, and durations
///          above @ref RT_AUDIO_FX_MAX_DELAY_SECONDS are capped.
/// @param seconds Requested duration in seconds.
/// @return Rounded frame count at @ref RT_AUDIO_FX_DEFAULT_RATE, at least one.
static int32_t seconds_to_frames(double seconds) {
    if (!isfinite(seconds) || seconds <= 0.0)
        seconds = 0.001;
    if (seconds > RT_AUDIO_FX_MAX_DELAY_SECONDS)
        seconds = RT_AUDIO_FX_MAX_DELAY_SECONDS;
    double frames = seconds * (double)RT_AUDIO_FX_DEFAULT_RATE;
    if (frames < 1.0)
        frames = 1.0;
    return (int32_t)(frames + 0.5);
}

/// @brief Allocate and initialize one zero-filled reverb delay line.
/// @param line State object to initialize.
/// @param frames Positive circular-buffer length in frames.
/// @return Non-zero on success, or zero for invalid input/allocation failure.
static int alloc_reverb_line(rt_reverb_line *line, int32_t frames) {
    if (!line || frames <= 0)
        return 0;
    line->buffer = (float *)calloc((size_t)frames, sizeof(float));
    if (!line->buffer)
        return 0;
    line->frames = frames;
    line->pos = 0;
    line->filter = 0.0f;
    return 1;
}

/// @brief Release a reverb line's circular buffer and reset its state.
/// @param line Line to clear; NULL is ignored.
static void free_reverb_line(rt_reverb_line *line) {
    if (!line)
        return;
    free(line->buffer);
    line->buffer = NULL;
    line->frames = 0;
    line->pos = 0;
    line->filter = 0.0f;
}

/// @brief Destroy an effect node and all kind-specific heap storage it owns.
/// @param fx Detached effect node; NULL is ignored.
static void free_fx(rt_audio_fx *fx) {
    if (!fx)
        return;
    if (fx->kind == RT_AUDIO_FX_DELAY) {
        free(fx->u.delay.buffer);
    } else if (fx->kind == RT_AUDIO_FX_REVERB) {
        for (int i = 0; i < 8; i++) {
            free_reverb_line(&fx->u.reverb.comb_l[i]);
            free_reverb_line(&fx->u.reverb.comb_r[i]);
        }
        for (int i = 0; i < 4; i++) {
            free_reverb_line(&fx->u.reverb.allpass_l[i]);
            free_reverb_line(&fx->u.reverb.allpass_r[i]);
        }
    }
    free(fx);
}

/// @brief Configure a low-pass, high-pass, or peaking biquad.
/// @details Coefficients use the fixed 44.1-kHz design rate. Frequency, Q, and
///          gain are normalized to stable ranges, coefficients are divided by
///          `a0`, and both channel histories are reset.
/// @param bq Biquad state to configure; NULL is ignored after input sanitation.
/// @param mode `0` for low-pass, `1` for high-pass, or any other value for
///        peaking EQ.
/// @param freq_hz Cutoff or center frequency in hertz.
/// @param q Resonance/bandwidth quality factor.
/// @param gain_db Peaking gain in decibels; ignored by low/high-pass modes.
static void biquad_set(rt_biquad_state *bq, int mode, double freq_hz, double q, double gain_db) {
    // Sanitize gain like the sibling effects sanitize their inputs
    // (VDOC-122): non-finite gain would poison the coefficients and delay
    // state, silencing the whole group. Clamp to a generous +/-40 dB.
    if (!isfinite(gain_db))
        gain_db = 0.0;
    else if (gain_db > 40.0)
        gain_db = 40.0;
    else if (gain_db < -40.0)
        gain_db = -40.0;
    if (!bq)
        return;
    if (!isfinite(freq_hz) || freq_hz < 10.0)
        freq_hz = 10.0;
    if (freq_hz > (double)RT_AUDIO_FX_DEFAULT_RATE * 0.45)
        freq_hz = (double)RT_AUDIO_FX_DEFAULT_RATE * 0.45;
    if (!isfinite(q) || q < 0.05)
        q = 0.707;
    if (q > 20.0)
        q = 20.0;

    double w0 = 2.0 * RT_AUDIO_FX_PI * freq_hz / (double)RT_AUDIO_FX_DEFAULT_RATE;
    double cw = cos(w0);
    double sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;

    if (mode == 0) {
        b0 = (1.0 - cw) * 0.5;
        b1 = 1.0 - cw;
        b2 = (1.0 - cw) * 0.5;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
    } else if (mode == 1) {
        b0 = (1.0 + cw) * 0.5;
        b1 = -(1.0 + cw);
        b2 = (1.0 + cw) * 0.5;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha;
    } else {
        double a = pow(10.0, gain_db / 40.0);
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cw;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cw;
        a2 = 1.0 - alpha / a;
    }

    if (fabs(a0) < 1.0e-12)
        a0 = 1.0;
    bq->b0 = (float)(b0 / a0);
    bq->b1 = (float)(b1 / a0);
    bq->b2 = (float)(b2 / a0);
    bq->a1 = (float)(a1 / a0);
    bq->a2 = (float)(a2 / a0);
    bq->z1_l = bq->z2_l = bq->z1_r = bq->z2_r = 0.0f;
}

/// @brief Process one sample through one channel of a stereo biquad.
/// @details Uses transposed direct-form state and sanitizes the returned output
///          against non-finite/denormal feedback.
/// @param bq Configured filter state.
/// @param in Input sample.
/// @param right Zero selects left-channel history; non-zero selects right.
/// @return Filtered sample.
static float biquad_sample(rt_biquad_state *bq, float in, int right) {
    float *z1 = right ? &bq->z1_r : &bq->z1_l;
    float *z2 = right ? &bq->z2_r : &bq->z2_l;
    float out = in * bq->b0 + *z1;
    *z1 = in * bq->b1 + *z2 - bq->a1 * out;
    *z2 = in * bq->b2 - bq->a2 * out;
    return sanitize_feedback(out);
}

/// @brief Filter an interleaved sample block in place with a stereo biquad.
/// @details Mono input uses the left history. For buffers with more than two
///          channels, only channels zero and one are processed.
/// @param bq Configured filter state.
/// @param samples Writable interleaved sample buffer.
/// @param frames Number of frames to process.
/// @param channels Number of interleaved channels per frame.
static void process_biquad(rt_biquad_state *bq, float *samples, int32_t frames, int32_t channels) {
    if (!bq || !samples || channels < 1)
        return;
    for (int32_t i = 0; i < frames; i++) {
        samples[(size_t)i * channels] = biquad_sample(bq, samples[(size_t)i * channels], 0);
        if (channels > 1)
            samples[(size_t)i * channels + 1] =
                biquad_sample(bq, samples[(size_t)i * channels + 1], 1);
    }
}

/// @brief Process an interleaved block through a stereo feedback delay.
/// @details Reads and writes the circular delay line in place, mixes wet and
///          dry signals, and mirrors mono input into the right delay state.
///          Channels beyond the first two are left unchanged.
/// @param delay Initialized delay state.
/// @param samples Writable interleaved sample buffer.
/// @param frames Number of frames to process.
/// @param channels Number of interleaved channels per frame.
static void process_delay(rt_delay_state *delay, float *samples, int32_t frames, int32_t channels) {
    if (!delay || !delay->buffer || !samples || delay->frames <= 0 || channels < 1)
        return;
    for (int32_t i = 0; i < frames; i++) {
        size_t sample_base = (size_t)i * (size_t)channels;
        size_t delay_base = (size_t)delay->pos * 2u;
        float in_l = samples[sample_base];
        float in_r = channels > 1 ? samples[sample_base + 1] : in_l;
        float delayed_l = delay->buffer[delay_base];
        float delayed_r = delay->buffer[delay_base + 1];
        samples[sample_base] = sanitize_feedback(in_l * delay->dry + delayed_l * delay->wet);
        if (channels > 1)
            samples[sample_base + 1] =
                sanitize_feedback(in_r * delay->dry + delayed_r * delay->wet);
        delay->buffer[delay_base] = sanitize_feedback(in_l + delayed_l * delay->feedback);
        delay->buffer[delay_base + 1] = sanitize_feedback(in_r + delayed_r * delay->feedback);
        delay->pos++;
        if (delay->pos >= delay->frames)
            delay->pos = 0;
    }
}

/// @brief Process one sample through a damped feedback comb line.
/// @param line Initialized circular delay line.
/// @param input Input sample.
/// @param room Feedback gain controlling decay.
/// @param damp One-pole damping coefficient.
/// @return Delayed sample read before the line is updated.
static float process_comb(rt_reverb_line *line, float input, float room, float damp) {
    float out = line->buffer[line->pos];
    line->filter = sanitize_feedback(out * (1.0f - damp) + line->filter * damp);
    line->buffer[line->pos] = sanitize_feedback(input + line->filter * room);
    line->pos++;
    if (line->pos >= line->frames)
        line->pos = 0;
    return out;
}

/// @brief Process one sample through a fixed-feedback all-pass line.
/// @param line Initialized circular delay line.
/// @param input Input sample.
/// @return Phase-dispersed output with unstable values sanitized.
static float process_allpass(rt_reverb_line *line, float input) {
    float buffered = line->buffer[line->pos];
    float out = -input + buffered;
    line->buffer[line->pos] = sanitize_feedback(input + buffered * 0.5f);
    line->pos++;
    if (line->pos >= line->frames)
        line->pos = 0;
    return sanitize_feedback(out);
}

/// @brief Process an interleaved block through the stereo reverb network.
/// @details Downmixes the first two input channels to a mono excitation, runs
///          eight parallel combs and four serial all-pass stages per side, then
///          blends wet and dry output. Channels beyond stereo are unchanged.
/// @param rv Initialized reverb state.
/// @param samples Writable interleaved sample buffer.
/// @param frames Number of frames to process.
/// @param channels Number of interleaved channels per frame.
static void process_reverb(rt_reverb_state *rv, float *samples, int32_t frames, int32_t channels) {
    if (!rv || !samples || channels < 1)
        return;
    for (int32_t i = 0; i < frames; i++) {
        size_t base = (size_t)i * (size_t)channels;
        float dry_l = samples[base];
        float dry_r = channels > 1 ? samples[base + 1] : dry_l;
        float mono = (dry_l + dry_r) * 0.5f;
        float wet_l = 0.0f;
        float wet_r = 0.0f;
        for (int c = 0; c < 8; c++) {
            wet_l += process_comb(&rv->comb_l[c], mono, rv->room, rv->damp);
            wet_r += process_comb(&rv->comb_r[c], mono, rv->room, rv->damp);
        }
        wet_l *= 0.125f;
        wet_r *= 0.125f;
        for (int a = 0; a < 4; a++) {
            wet_l = process_allpass(&rv->allpass_l[a], wet_l);
            wet_r = process_allpass(&rv->allpass_r[a], wet_r);
        }
        samples[base] = sanitize_feedback(dry_l * rv->dry + wet_l * rv->wet);
        if (channels > 1)
            samples[base + 1] = sanitize_feedback(dry_r * rv->dry + wet_r * rv->wet);
    }
}

/// @brief Append a prepared effect node to a group's ordered insert chain.
/// @details Assigns a positive process-wide identifier under the registry lock
///          and preserves insertion order. Identifier wrap restarts subsequent
///          allocation at one.
/// @param group Numerically valid mix-group index.
/// @param fx Heap-allocated effect node. On success, ownership transfers to the
///        group chain.
/// @return Assigned effect identifier, or `-1` for invalid input.
static int64_t append_fx(int64_t group, rt_audio_fx *fx) {
    if (!group_valid(group) || !fx)
        return -1;
    fx_lock();
    fx->id = g_next_fx_id++;
    if (g_next_fx_id <= 0)
        g_next_fx_id = 1;
    fx->next = NULL;
    rt_audio_fx **tail = &g_group_fx[group];
    while (*tail)
        tail = &(*tail)->next;
    *tail = fx;
    int64_t id = fx->id;
    fx_unlock();
    return id;
}

/// @brief Allocate and append a low-pass biquad insert.
/// @param group Numerically valid mix-group index.
/// @param cutoff_hz Cutoff frequency in hertz.
/// @param q Resonance/quality factor.
/// @return Positive effect identifier, or `-1` on allocation/validation failure.
int64_t rt_audio_fx_add_lowpass(int64_t group, double cutoff_hz, double q) {
    rt_audio_fx *fx = (rt_audio_fx *)calloc(1, sizeof(rt_audio_fx));
    if (!fx)
        return -1;
    fx->kind = RT_AUDIO_FX_BIQUAD;
    biquad_set(&fx->u.biquad, 0, cutoff_hz, q, 0.0);
    return append_fx(group, fx);
}

/// @brief Allocate and append a high-pass biquad insert.
/// @param group Numerically valid mix-group index.
/// @param cutoff_hz Cutoff frequency in hertz.
/// @param q Resonance/quality factor.
/// @return Positive effect identifier, or `-1` on allocation/validation failure.
int64_t rt_audio_fx_add_highpass(int64_t group, double cutoff_hz, double q) {
    rt_audio_fx *fx = (rt_audio_fx *)calloc(1, sizeof(rt_audio_fx));
    if (!fx)
        return -1;
    fx->kind = RT_AUDIO_FX_BIQUAD;
    biquad_set(&fx->u.biquad, 1, cutoff_hz, q, 0.0);
    return append_fx(group, fx);
}

/// @brief Allocate and append a peaking equalizer insert.
/// @param group Numerically valid mix-group index.
/// @param freq_hz Center frequency in hertz.
/// @param q Bandwidth/quality factor.
/// @param gain_db Center-frequency gain in decibels.
/// @return Positive effect identifier, or `-1` on allocation/validation failure.
int64_t rt_audio_fx_add_peaking(int64_t group, double freq_hz, double q, double gain_db) {
    rt_audio_fx *fx = (rt_audio_fx *)calloc(1, sizeof(rt_audio_fx));
    if (!fx)
        return -1;
    fx->kind = RT_AUDIO_FX_BIQUAD;
    biquad_set(&fx->u.biquad, 2, freq_hz, q, gain_db);
    return append_fx(group, fx);
}

/// @brief Allocate and append a stereo feedback-delay insert.
/// @details Delay storage is sized at the fixed DSP design rate. Feedback is
///          clamped to `[0, 0.95]`, wet mix to `[0, 1]`, and dry mix is its
///          complement.
/// @param group Numerically valid mix-group index.
/// @param delay_ms Requested delay in milliseconds.
/// @param feedback Requested feedback fraction.
/// @param wet Requested wet-signal fraction.
/// @return Positive effect identifier, or `-1` on allocation/validation failure.
int64_t rt_audio_fx_add_delay(int64_t group, double delay_ms, double feedback, double wet) {
    rt_audio_fx *fx = (rt_audio_fx *)calloc(1, sizeof(rt_audio_fx));
    if (!fx)
        return -1;
    fx->kind = RT_AUDIO_FX_DELAY;
    int32_t frames = seconds_to_frames(delay_ms / 1000.0);
    fx->u.delay.buffer = (float *)calloc((size_t)frames * 2u, sizeof(float));
    if (!fx->u.delay.buffer) {
        free(fx);
        return -1;
    }
    fx->u.delay.frames = frames;
    fx->u.delay.feedback = clampf_range((float)feedback, 0.0f, 0.95f);
    fx->u.delay.wet = clampf_range((float)wet, 0.0f, 1.0f);
    fx->u.delay.dry = 1.0f - fx->u.delay.wet;
    return append_fx(group, fx);
}

/// @brief Allocate and append a stereo Schroeder-style reverb insert.
/// @details Creates fixed comb/all-pass delay networks for both channels and
///          normalizes room size, damping, and wet mix into stable internal
///          ranges. Any partial allocation is released on failure.
/// @param group Numerically valid mix-group index.
/// @param room_size Requested normalized room size.
/// @param damping Requested normalized high-frequency damping.
/// @param wet Requested normalized wet-signal mix.
/// @return Positive effect identifier, or `-1` on allocation/validation failure.
int64_t rt_audio_fx_add_reverb(int64_t group, double room_size, double damping, double wet) {
    static const int comb_l[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int comb_r[8] = {1139, 1211, 1300, 1379, 1445, 1514, 1580, 1640};
    static const int all_l[4] = {556, 441, 341, 225};
    static const int all_r[4] = {579, 464, 364, 248};
    rt_audio_fx *fx = (rt_audio_fx *)calloc(1, sizeof(rt_audio_fx));
    if (!fx)
        return -1;
    fx->kind = RT_AUDIO_FX_REVERB;
    fx->u.reverb.room = 0.55f + clampf_range((float)room_size, 0.0f, 1.0f) * 0.4f;
    fx->u.reverb.damp = clampf_range((float)damping, 0.0f, 1.0f) * 0.8f;
    fx->u.reverb.wet = clampf_range((float)wet, 0.0f, 1.0f);
    fx->u.reverb.dry = 1.0f - fx->u.reverb.wet;
    for (int i = 0; i < 8; i++) {
        if (!alloc_reverb_line(&fx->u.reverb.comb_l[i], comb_l[i]) ||
            !alloc_reverb_line(&fx->u.reverb.comb_r[i], comb_r[i])) {
            free_fx(fx);
            return -1;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (!alloc_reverb_line(&fx->u.reverb.allpass_l[i], all_l[i]) ||
            !alloc_reverb_line(&fx->u.reverb.allpass_r[i], all_r[i])) {
            free_fx(fx);
            return -1;
        }
    }
    return append_fx(group, fx);
}

/// @brief Update a reverb insert's parameters in place.
/// @details Same clamps as rt_audio_fx_add_reverb; the delay lines are kept so
///   eased parameter sweeps (reverb zones) stay click-free and allocation-free.
/// @param group Numerically valid mix-group index.
/// @param fx_id Identifier of an existing reverb insert.
/// @param room_size Requested normalized room size.
/// @param damping Requested normalized high-frequency damping.
/// @param wet Requested normalized wet-signal mix.
void rt_audio_fx_set_reverb_params(
    int64_t group, int64_t fx_id, double room_size, double damping, double wet) {
    if (!group_valid(group) || fx_id <= 0)
        return;
    fx_lock();
    for (rt_audio_fx *fx = g_group_fx[group]; fx; fx = fx->next) {
        if (fx->id == fx_id && fx->kind == RT_AUDIO_FX_REVERB) {
            fx->u.reverb.room = 0.55f + clampf_range((float)room_size, 0.0f, 1.0f) * 0.4f;
            fx->u.reverb.damp = clampf_range((float)damping, 0.0f, 1.0f) * 0.8f;
            fx->u.reverb.wet = clampf_range((float)wet, 0.0f, 1.0f);
            fx->u.reverb.dry = 1.0f - fx->u.reverb.wet;
            break;
        }
    }
    fx_unlock();
}

/// @brief Change whether an effect node participates in processing.
/// @param group Numerically valid mix-group index.
/// @param fx_id Positive identifier of an effect in the group.
/// @param bypass Non-zero to bypass the effect; zero to enable processing.
void rt_audio_fx_set_bypass(int64_t group, int64_t fx_id, int8_t bypass) {
    if (!group_valid(group) || fx_id <= 0)
        return;
    fx_lock();
    for (rt_audio_fx *fx = g_group_fx[group]; fx; fx = fx->next) {
        if (fx->id == fx_id) {
            fx->bypass = bypass ? 1 : 0;
            break;
        }
    }
    fx_unlock();
}

/// @brief Remove and destroy one effect from a group chain.
/// @details Unlinks the matching node under the registry lock, then frees its
///          potentially large DSP storage after releasing the lock. Missing or
///          invalid identifiers are ignored.
/// @param group Numerically valid mix-group index.
/// @param fx_id Positive effect identifier.
void rt_audio_fx_remove(int64_t group, int64_t fx_id) {
    if (!group_valid(group) || fx_id <= 0)
        return;
    fx_lock();
    rt_audio_fx **cur = &g_group_fx[group];
    while (*cur) {
        if ((*cur)->id == fx_id) {
            rt_audio_fx *dead = *cur;
            *cur = dead->next;
            fx_unlock();
            free_fx(dead);
            return;
        }
        cur = &(*cur)->next;
    }
    fx_unlock();
}

/// @brief Detach and destroy every effect in one mix-group chain.
/// @details The complete list is detached atomically, then nodes and DSP
///          buffers are freed outside the registry lock.
/// @param group Numerically valid mix-group index.
void rt_audio_fx_clear_group(int64_t group) {
    if (!group_valid(group))
        return;
    fx_lock();
    rt_audio_fx *cur = g_group_fx[group];
    g_group_fx[group] = NULL;
    fx_unlock();
    while (cur) {
        rt_audio_fx *next = cur->next;
        free_fx(cur);
        cur = next;
    }
}

/// @brief Clear every fixed mix-group effect chain.
/// @details Delegates each group to @ref rt_audio_fx_clear_group so large DSP
///          allocations are released outside the registry lock.
void rt_audio_fx_clear_all(void) {
    for (int64_t group = 0; group < RT_MIXGROUP_MAX_GROUPS; group++)
        rt_audio_fx_clear_group(group);
}

/// @brief Check whether a group has any enabled effect.
/// @details Uses the non-blocking mixer lock. Contention is reported as no
///          effects so a real-time caller can pass the audio through rather
///          than waiting on a control-thread mutation.
/// @param group Numerically valid mix-group index.
/// @return Non-zero when an unbypassed effect is present; zero for an empty or
///         invalid chain, or when the registry lock is contended.
int rt_audio_fx_group_has_effects(int64_t group) {
    if (!group_valid(group))
        return 0;
    if (!fx_try_lock())
        return 0;
    int result = 0;
    for (rt_audio_fx *fx = g_group_fx[group]; fx; fx = fx->next) {
        if (!fx->bypass) {
            result = 1;
            break;
        }
    }
    fx_unlock();
    return result;
}

/// @brief Run a mix group's enabled inserts over a sample block in place.
/// @details Acquires the registry only with a try-lock; invalid input or lock
///          contention leaves the buffer unchanged. Effects execute in
///          insertion order. @p sample_rate is currently informational because
///          coefficient and delay state is designed at 44.1 kHz.
/// @param group Numerically valid mix-group index.
/// @param samples Writable interleaved floating-point sample buffer.
/// @param frames Positive number of frames to process.
/// @param channels Positive number of channels per frame.
/// @param sample_rate Backend sample rate in hertz; currently unused.
void rt_audio_fx_process_group(
    int64_t group, float *samples, int32_t frames, int32_t channels, int32_t sample_rate) {
    (void)sample_rate;
    if (!group_valid(group) || !samples || frames <= 0 || channels <= 0)
        return;
    if (!fx_try_lock())
        return;
    for (rt_audio_fx *fx = g_group_fx[group]; fx; fx = fx->next) {
        if (fx->bypass)
            continue;
        if (fx->kind == RT_AUDIO_FX_BIQUAD) {
            process_biquad(&fx->u.biquad, samples, frames, channels);
        } else if (fx->kind == RT_AUDIO_FX_DELAY) {
            process_delay(&fx->u.delay, samples, frames, channels);
        } else if (fx->kind == RT_AUDIO_FX_REVERB) {
            process_reverb(&fx->u.reverb, samples, frames, channels);
        }
    }
    fx_unlock();
}
