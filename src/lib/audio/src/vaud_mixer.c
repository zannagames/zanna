//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// ZannaAUD Software Mixer
//
// Combines multiple audio voices and music streams into a single stereo output.
// The mixer is called from the audio thread to fill platform audio buffers.
//
// Key features:
// - Up to VAUD_MAX_VOICES simultaneous sound effects
// - Per-voice volume and stereo panning
// - Music streaming with multiple buffer support
// - Clipping prevention via soft limiting
//
// Mixing algorithm:
// 1. Clear output buffer to zero
// 2. For each active voice, add scaled samples to output
// 3. Add active music streams
// 4. Apply master volume
// 5. Soft clip to prevent distortion
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Software audio mixer for ZannaAUD.
/// @details Implements the allocation-free realtime render path shared by all
///          platform devices. The mixer advances duck envelopes, routes logical
///          groups through optional effects, mixes sound and streaming sources
///          into saturating 32-bit buses, converts the master bus to signed-16
///          PCM, and caches a degraded fallback period for mutex contention.

#include "vaud_internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Mixer Constants
//===----------------------------------------------------------------------===//

/// @brief Maximum amplitude before soft clipping engages.
#define VAUD_CLIP_THRESHOLD 28000

/// @brief Soft clip knee factor (higher = softer knee).
#define VAUD_CLIP_KNEE 0.25f

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

/// @brief Apply soft clipping to prevent harsh distortion.
/// @details Uses a tanh-like curve above the threshold to gently limit peaks.
/// @param sample Input sample (may exceed 16-bit range).
/// @return Clipped sample in 16-bit range.
static inline int16_t soft_clip(int32_t sample) {
    if (sample > VAUD_CLIP_THRESHOLD) {
        double excess = (double)sample - (double)VAUD_CLIP_THRESHOLD;
        double compressed = (double)VAUD_CLIP_THRESHOLD + excess * (double)VAUD_CLIP_KNEE;
        sample = (int32_t)compressed;
    } else if (sample < -VAUD_CLIP_THRESHOLD) {
        double excess = -(double)sample - (double)VAUD_CLIP_THRESHOLD;
        double compressed = -((double)VAUD_CLIP_THRESHOLD + excess * (double)VAUD_CLIP_KNEE);
        sample = (int32_t)compressed;
    }

    /* Hard clamp as final safety */
    if (sample > 32767)
        return 32767;
    if (sample < -32768)
        return -32768;
    return (int16_t)sample;
}

/// @brief Clamp a mixer volume multiplier to the public 0..1 range.
/// @details Public setters already clamp these fields, but the realtime mixer
///          also sanitizes them before arithmetic so corrupted internal state or
///          cross-thread torn float reads cannot create undefined integer
///          overflow in the accumulator.
/// @param volume Candidate volume multiplier.
/// @return Finite volume in the inclusive range [0, 1].
static float vaud_mixer_unit_volume(float volume) {
    if (!isfinite(volume) || volume <= 0.0f)
        return 0.0f;
    if (volume > 1.0f)
        return 1.0f;
    return volume;
}

/// @brief Convert a sanitized floating gain to 8.8 fixed-point.
/// @param gain Floating gain, expected in [0, 1].
/// @return Fixed-point gain with 256 representing unity.
static int32_t vaud_mixer_gain_to_fixed(float gain) {
    if (!isfinite(gain) || gain <= 0.0f)
        return 0;
    if (gain >= 1.0f)
        return 256;
    return (int32_t)(gain * 256.0f + 0.5f);
}

/// @brief Saturating add for 32-bit mixer accumulators.
/// @param lhs Existing accumulator value.
/// @param rhs Sample contribution to add.
/// @return Sum clamped to INT32_MIN..INT32_MAX.
static int32_t vaud_mixer_saturating_add_i32(int32_t lhs, int32_t rhs) {
    int64_t sum = (int64_t)lhs + (int64_t)rhs;
    if (sum > INT32_MAX)
        return INT32_MAX;
    if (sum < INT32_MIN)
        return INT32_MIN;
    return (int32_t)sum;
}

/// @brief Convert a float effect output sample to a bounded accumulator sample.
/// @details Group effects exchange normalized float samples.  Non-finite values
///          are silenced and huge finite values are clamped before conversion so
///          third-party effect callbacks cannot trigger undefined casts or
///          accumulator overflow.
/// @param scaled Effect sample already scaled to 16-bit PCM units.
/// @return Rounded int32 sample contribution.
static int32_t vaud_mixer_float_to_accum_sample(float scaled) {
    if (!isfinite(scaled))
        return 0;
    if (scaled > (float)INT32_MAX)
        return INT32_MAX;
    if (scaled < (float)INT32_MIN)
        return INT32_MIN;
    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

/// @brief Clear an interleaved device buffer after validating its byte count.
/// @param output Destination signed-16 PCM buffer.
/// @param frames Number of stereo frames to clear.
static void vaud_zero_output(int16_t *output, int32_t frames) {
    size_t output_bytes = 0;
    if (vaud_pcm_s16_buffer_size(frames, VAUD_CHANNELS, &output_bytes))
        memset(output, 0, output_bytes);
}

/// @brief Emit an attenuated copy of the last rendered period after a lock miss.
/// @details A missed mixer lock used to become a full silent period. Reusing the
///          previous period at reduced gain is still deterministic and
///          allocation-free, but avoids a hard discontinuity on the output bus.
/// @param ctx Audio context containing the cached render period.
/// @param output Destination interleaved stereo PCM buffer.
/// @param frames Number of frames to fill.
static void vaud_mixer_render_lock_miss_fallback(vaud_context_t ctx,
                                                 int16_t *output,
                                                 int32_t frames) {
    if (!ctx || !output || frames <= 0 || ctx->last_output_frames <= 0) {
        vaud_zero_output(output, frames);
        return;
    }

    int32_t valid = ctx->last_output_frames;
    if (valid > frames)
        valid = frames;
    size_t samples = (size_t)valid * (size_t)VAUD_CHANNELS;
    for (size_t i = 0; i < samples; i++)
        output[i] = (int16_t)(ctx->last_output_buf[i] / 2);

    if (valid < frames)
        vaud_zero_output(output + (size_t)valid * VAUD_CHANNELS, frames - valid);

    size_t output_bytes = 0;
    if (vaud_pcm_s16_buffer_size(frames, VAUD_CHANNELS, &output_bytes)) {
        memcpy(ctx->last_output_buf, output, output_bytes);
        ctx->last_output_frames = frames;
    }
}

/// @brief Cache the latest successfully mixed period for lock-miss fallback.
/// @param ctx Audio context containing the fallback buffer.
/// @param output Interleaved stereo PCM period just rendered by the mixer.
/// @param frames Number of frames in @p output.
static void vaud_mixer_cache_output(vaud_context_t ctx, const int16_t *output, int32_t frames) {
    if (!ctx || !output || frames <= 0 || frames > VAUD_BUFFER_FRAMES)
        return;
    size_t output_bytes = 0;
    if (!vaud_pcm_s16_buffer_size(frames, VAUD_CHANNELS, &output_bytes))
        return;
    memcpy(ctx->last_output_buf, output, output_bytes);
    ctx->last_output_frames = frames;
}

/// @brief Calculate left/right gain from pan value.
/// @param pan Pan value (-1.0 = left, 0.0 = center, 1.0 = right).
/// @param source_channels Original source channel count (1 = mono, 2 = stereo).
/// @param left_gain Output: left channel gain.
/// @param right_gain Output: right channel gain.
static void calculate_pan_gains(float pan,
                                int32_t source_channels,
                                float *left_gain,
                                float *right_gain) {
    if (!isfinite(pan))
        pan = 0.0f;
    if (pan < -1.0f)
        pan = -1.0f;
    if (pan > 1.0f)
        pan = 1.0f;

    if (source_channels == 1) {
        /* Equal-power panning for mono sources keeps center playback perceptually stable. */
        float angle = (pan + 1.0f) * 0.78539816339f; /* (pan + 1) * pi / 4 */
        *left_gain = cosf(angle);
        *right_gain = sinf(angle);
        return;
    }

    /* Stereo sources use balance semantics: center is unity, pan attenuates the far side. */
    *left_gain = (pan > 0.0f) ? (1.0f - pan) : 1.0f;
    *right_gain = (pan < 0.0f) ? (1.0f + pan) : 1.0f;
}

//===----------------------------------------------------------------------===//
// Voice Mixing
//===----------------------------------------------------------------------===//

/// @brief Combined duck gain for every rule targeting @p group_id.
/// @details Rules are updated once per render pass by
///          mixer_update_duck_rules(); this is a pure product lookup.
/// @param ctx Context containing the current duck-rule envelopes.
/// @param group_id Logical mix group whose target rules are combined.
/// @return Product of all matching rule gains, or unity when none match.
static float mixer_group_duck_gain(vaud_context_t ctx, int64_t group_id) {
    float gain = 1.0f;
    for (int32_t i = 0; i < ctx->duck_rule_count; i++) {
        if (ctx->duck_rules[i].target_group == group_id)
            gain *= ctx->duck_rules[i].gain;
    }
    return gain;
}

/// @brief Advance the sidechain duck envelopes for this render chunk.
/// @details A rule is "triggered" while any playing, audible voice (or music
///          stream) lives in its trigger group. Triggered rules ease their
///          gain toward (1 - amount) at the attack rate; idle rules recover
///          toward unity at the release rate. Rates convert the configured
///          attack/release seconds into a per-chunk exponential step so the
///          envelope shape is independent of the backend period size.
/// @param ctx Context whose voices, streams, and duck rules are inspected.
/// @param frames Number of output frames represented by this envelope step.
static void mixer_update_duck_rules(vaud_context_t ctx, int32_t frames) {
    if (ctx->duck_rule_count <= 0)
        return;
    float chunk_sec = (float)frames / (float)VAUD_SAMPLE_RATE;

    for (int32_t r = 0; r < ctx->duck_rule_count; r++) {
        vaud_duck_rule *rule = &ctx->duck_rules[r];

        int triggered = 0;
        for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
            const vaud_voice *voice = &ctx->voices[i];
            if (voice->state == VAUD_VOICE_PLAYING && voice->sound &&
                voice->group_id == rule->trigger_group && voice->volume > 0.001f) {
                triggered = 1;
                break;
            }
        }
        if (!triggered) {
            for (int32_t i = 0; i < ctx->music_count; i++) {
                vaud_music_t music = ctx->active_music[i];
                if (music && music->state == VAUD_MUSIC_PLAYING &&
                    music->group_id == rule->trigger_group && music->volume > 0.001f) {
                    triggered = 1;
                    break;
                }
            }
        }

        float target = triggered ? (1.0f - rule->amount) : 1.0f;
        float tau = triggered ? rule->attack_sec : rule->release_sec;
        if (tau < 0.001f)
            tau = 0.001f;
        float k = 1.0f - expf(-chunk_sec / tau);
        rule->gain += (target - rule->gain) * k;
        if (rule->gain < 0.0f)
            rule->gain = 0.0f;
        if (rule->gain > 1.0f)
            rule->gain = 1.0f;
    }
}

/// @brief One-pole lowpass coefficient for a cutoff at the mixer sample rate.
/// @param cutoff_hz Positive cutoff frequency in hertz.
/// @return Stable per-sample interpolation coefficient clamped to [0.0001, 1].
static inline float mixer_lowpass_coeff(float cutoff_hz) {
    float a = 1.0f - expf(-6.2831853f * cutoff_hz / (float)VAUD_SAMPLE_RATE);
    if (a < 0.0001f)
        a = 0.0001f;
    if (a > 1.0f)
        a = 1.0f;
    return a;
}

/// @brief Mix a single voice into the output buffer.
/// @details Two paths: a fixed-point fast path for pass-through voices
///          (pitch 1.0, no filtering — the overwhelmingly common case), and a
///          general path with a fractional resampling cursor (linear
///          interpolation) plus an optional one-pole lowpass for direct
///          cutoff and occlusion filtering. Occlusion changes are smoothed
///          (~80 ms) per chunk to avoid zipper noise.
/// @param voice Voice to mix.
/// @param output Output buffer (stereo interleaved).
/// @param frames Number of frames to mix.
/// @param master_vol Master volume multiplier (duck gain pre-applied by caller).
/// @return 1 if voice is still active, 0 if finished.
static int mix_voice(vaud_voice *voice, int32_t *output, int32_t frames, float master_vol) {
    if (!voice || voice->state != VAUD_VOICE_PLAYING || !voice->sound)
        return 0;

    vaud_sound_t sound = voice->sound;
    const int16_t *samples = sound->samples;
    int64_t sound_frames = sound->frame_count;
    int64_t pos = voice->position;

    /* Smooth the occlusion amount toward its target once per chunk (~80 ms
     * time constant) so gameplay-driven occlusion flips never click. */
    if (voice->occlusion_smooth != voice->occlusion_target) {
        float k = 1.0f - expf(-(float)frames / (0.080f * (float)VAUD_SAMPLE_RATE));
        voice->occlusion_smooth += (voice->occlusion_target - voice->occlusion_smooth) * k;
        if (voice->occlusion_smooth < 0.0005f && voice->occlusion_target == 0.0f)
            voice->occlusion_smooth = 0.0f;
    }

    float left_gain, right_gain;
    calculate_pan_gains(voice->pan, sound->source_channels, &left_gain, &right_gain);

    float vol = vaud_mixer_unit_volume(voice->volume) * vaud_mixer_unit_volume(master_vol);

    /* Occlusion attenuates as well as muffles: up to -6 dB at full occlusion. */
    float occ = voice->occlusion_smooth;
    if (occ > 0.0f)
        vol *= 1.0f - 0.5f * occ;

    left_gain *= vol;
    right_gain *= vol;

    /* Resolve the effective lowpass cutoff: the occlusion sweep (22 kHz down
     * to ~800 Hz, perceptual curve) combined with any direct cutoff — the
     * lower (more muffled) of the two wins. */
    float cutoff = 0.0f;
    if (occ > 0.001f)
        cutoff = 22000.0f * powf(800.0f / 22000.0f, occ);
    if (voice->lowpass_cutoff > 0.0f && (cutoff <= 0.0f || voice->lowpass_cutoff < cutoff))
        cutoff = voice->lowpass_cutoff;

    float pitch = voice->pitch > 0.0f ? voice->pitch : 1.0f;
    int needs_general_path = (pitch != 1.0f) || (cutoff > 0.0f);

    /* Convert to fixed-point for efficiency */
    int32_t left_gain_fp = vaud_mixer_gain_to_fixed(left_gain);
    int32_t right_gain_fp = vaud_mixer_gain_to_fixed(right_gain);

    /* Per-voice RMS metering: pre-gain source level so distance attenuation
     * never closes a lip-synced mouth. Accumulated only when enabled. */
    float meter_sum_sq = 0.0f;
    int32_t meter_samples = 0;

    if (!needs_general_path) {
        for (int32_t i = 0; i < frames; i++) {
            if (pos >= sound_frames) {
                if (voice->loop) {
                    pos = 0;
                } else {
                    voice->state = VAUD_VOICE_INACTIVE;
                    voice->sound = NULL;
                    voice->position = pos;
                    return 0;
                }
            }

            /* Source is stereo interleaved */
            int16_t src_left = samples[pos * 2];
            int16_t src_right = samples[pos * 2 + 1];

            /* Apply volume and panning, accumulate into output */
            output[i * 2] =
                vaud_mixer_saturating_add_i32(output[i * 2], (src_left * left_gain_fp) >> 8);
            output[i * 2 + 1] =
                vaud_mixer_saturating_add_i32(output[i * 2 + 1], (src_right * right_gain_fp) >> 8);

            if (voice->metering) {
                float nl = (float)src_left * (1.0f / 32768.0f);
                float nr = (float)src_right * (1.0f / 32768.0f);
                meter_sum_sq += nl * nl + nr * nr;
                meter_samples += 2;
            }
            pos++;
        }

        if (voice->metering)
            voice->level =
                meter_samples > 0 ? sqrtf(meter_sum_sq / (float)meter_samples) : 0.0f;
        voice->position = pos;
        /* Keep the fractional cursor trailing the integer one so a later
         * pitch/filter change resumes from the right place. */
        voice->frac_pos = (double)pos;
        return 1;
    }

    /* General path: fractional cursor with linear interpolation + optional
     * one-pole lowpass. Sync the cursor forward if the fast path advanced it. */
    double fpos = voice->frac_pos;
    if ((double)pos > fpos)
        fpos = (double)pos;

    float lp_a = cutoff > 0.0f ? mixer_lowpass_coeff(cutoff) : 1.0f;
    float lp_l = voice->lp_state_l;
    float lp_r = voice->lp_state_r;
    double step = (double)pitch;

    for (int32_t i = 0; i < frames; i++) {
        if (fpos >= (double)sound_frames) {
            if (voice->loop) {
                fpos -= (double)sound_frames;
                if (fpos >= (double)sound_frames)
                    fpos = 0.0; /* pathological step > length */
            } else {
                voice->state = VAUD_VOICE_INACTIVE;
                voice->sound = NULL;
                voice->position = sound_frames;
                voice->frac_pos = (double)sound_frames;
                voice->lp_state_l = lp_l;
                voice->lp_state_r = lp_r;
                return 0;
            }
        }

        int64_t i0 = (int64_t)fpos;
        double frac = fpos - (double)i0;
        int64_t i1 = i0 + 1;
        if (i1 >= sound_frames)
            i1 = voice->loop ? 0 : sound_frames - 1;

        float src_left = (float)samples[i0 * 2] +
                         ((float)samples[i1 * 2] - (float)samples[i0 * 2]) * (float)frac;
        float src_right = (float)samples[i0 * 2 + 1] +
                          ((float)samples[i1 * 2 + 1] - (float)samples[i0 * 2 + 1]) * (float)frac;

        if (cutoff > 0.0f) {
            lp_l += lp_a * (src_left - lp_l);
            lp_r += lp_a * (src_right - lp_r);
            src_left = lp_l;
            src_right = lp_r;
        }

        output[i * 2] = vaud_mixer_saturating_add_i32(
            output[i * 2], ((int32_t)src_left * left_gain_fp) >> 8);
        output[i * 2 + 1] = vaud_mixer_saturating_add_i32(
            output[i * 2 + 1], ((int32_t)src_right * right_gain_fp) >> 8);

        if (voice->metering) {
            float nl = src_left * (1.0f / 32768.0f);
            float nr = src_right * (1.0f / 32768.0f);
            meter_sum_sq += nl * nl + nr * nr;
            meter_samples += 2;
        }
        fpos += step;
    }

    if (voice->metering)
        voice->level = meter_samples > 0 ? sqrtf(meter_sum_sq / (float)meter_samples) : 0.0f;
    voice->frac_pos = fpos;
    voice->position = (int64_t)fpos;
    voice->lp_state_l = lp_l;
    voice->lp_state_r = lp_r;
    return 1;
}

//===----------------------------------------------------------------------===//
// Music Mixing
//===----------------------------------------------------------------------===//

/// @brief Mix music stream into the output buffer.
/// @details Consumes ready decoded-buffer slots without blocking or decoding.
///          Empty slots leave the remainder of the destination untouched and
///          publish loop/stop state for the control-thread refill pass.
/// @param music Music stream to mix.
/// @param output Output buffer (stereo interleaved).
/// @param frames Number of frames to mix.
/// @param master_vol Master volume multiplier.
static void mix_music(vaud_music_t music, int32_t *output, int32_t frames, float master_vol) {
    if (!music || music->state != VAUD_MUSIC_PLAYING)
        return;
    if (music->buffer_refilling[music->current_buffer])
        return;

    float vol = vaud_mixer_unit_volume(music->volume) * vaud_mixer_unit_volume(master_vol);
    int32_t vol_fp = vaud_mixer_gain_to_fixed(vol);

    int32_t frames_remaining = frames;
    int32_t output_offset = 0;

    while (frames_remaining > 0) {
        if (music->buffer_position >= music->buffer_frames[music->current_buffer]) {
            music->buffer_frames[music->current_buffer] = 0;
            music->buffer_position = 0;
            music->current_buffer = (music->current_buffer + 1) % VAUD_MUSIC_BUFFER_COUNT;

            if (music->buffer_refilling[music->current_buffer])
                return;

            if (music->buffer_frames[music->current_buffer] <= 0) {
                if (music->loop && music->stream_eof) {
                    music->stream_loop_pending = 1;
                }

                if (music->buffer_frames[music->current_buffer] <= 0 && !music->loop &&
                    music->stream_eof) {
                    music->state = VAUD_MUSIC_STOPPED;
                }
                if (music->buffer_frames[music->current_buffer] <= 0)
                    return;
            }
        }

        /* Mix from current buffer */
        int16_t *src = music->buffers[music->current_buffer];
        int32_t available = music->buffer_frames[music->current_buffer] - music->buffer_position;
        int32_t to_mix = (frames_remaining < available) ? frames_remaining : available;

        int32_t src_offset = music->buffer_position * 2; /* Stereo */
        for (int32_t i = 0; i < to_mix; i++) {
            int16_t left = src[src_offset + i * 2];
            int16_t right = src[src_offset + i * 2 + 1];

            output[(output_offset + i) * 2] = vaud_mixer_saturating_add_i32(
                output[(output_offset + i) * 2], (left * vol_fp) >> 8);
            output[(output_offset + i) * 2 + 1] = vaud_mixer_saturating_add_i32(
                output[(output_offset + i) * 2 + 1], (right * vol_fp) >> 8);
        }

        music->buffer_position += to_mix;
        music->position += to_mix;
        output_offset += to_mix;
        frames_remaining -= to_mix;
    }
}

/// @brief Test membership in a bounded unsorted logical-group array.
/// @param groups Array containing @p count group identifiers.
/// @param count Number of initialized entries in @p groups.
/// @param group_id Identifier to find.
/// @return 1 when present; otherwise 0.
static int group_list_contains(const int64_t *groups, int32_t count, int64_t group_id) {
    for (int32_t i = 0; i < count; i++) {
        if (groups[i] == group_id)
            return 1;
    }
    return 0;
}

/// @brief Append a group identifier if it is unique and capacity remains.
/// @param groups Destination group array.
/// @param count In/out number of initialized entries.
/// @param cap Maximum number of entries in @p groups.
/// @param group_id Identifier to add.
static void group_list_add(int64_t *groups, int32_t *count, int32_t cap, int64_t group_id) {
    if (!groups || !count || *count >= cap)
        return;
    if (group_list_contains(groups, *count, group_id))
        return;
    groups[*count] = group_id;
    *count += 1;
}

/// @brief Collect active logical groups that may need effects processing.
/// @details The caller must hold the context mutex. This helper deliberately
///          does not call the user query hook; the realtime render path filters
///          candidates outside the context lock before mixing.
/// @param ctx Context whose active voices and streams are scanned.
/// @param groups Destination array for unique logical group identifiers.
/// @param cap Capacity of @p groups.
/// @return Number of candidate identifiers written.
static int32_t collect_effect_group_candidates_locked(vaud_context_t ctx,
                                                      int64_t *groups,
                                                      int32_t cap) {
    int32_t count = 0;
    if (!ctx || !groups || cap <= 0 || !ctx->group_effects_process)
        return 0;

    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        vaud_voice *voice = &ctx->voices[i];
        if (voice->state == VAUD_VOICE_PLAYING && voice->sound) {
            group_list_add(groups, &count, cap, voice->group_id);
        }
    }

    for (int32_t i = 0; i < ctx->music_count; i++) {
        vaud_music_t music = ctx->active_music[i];
        if (music && music->state == VAUD_MUSIC_PLAYING) {
            group_list_add(groups, &count, cap, music->group_id);
        }
    }

    return count;
}

/// @brief Apply the optional user query hook to candidate effect groups.
/// @details The caller holds the audio context mutex so the query callback,
///          process callback, and their userdata cannot be replaced while a
///          render pass is using them. If no query hook is installed, every
///          candidate group is treated as effect-enabled.
/// @param query Optional predicate invoked once for each candidate.
/// @param userdata Opaque state passed to @p query.
/// @param candidates Unique active groups available for selection.
/// @param candidate_count Number of entries in @p candidates.
/// @param groups Destination array for accepted unique identifiers.
/// @param cap Capacity of @p groups.
/// @return Number of accepted groups written.
static int32_t filter_effect_groups_locked(vaud_group_effects_query_fn query,
                                           void *userdata,
                                           const int64_t *candidates,
                                           int32_t candidate_count,
                                           int64_t *groups,
                                           int32_t cap) {
    int32_t count = 0;
    if (!candidates || !groups || candidate_count <= 0 || cap <= 0)
        return 0;
    for (int32_t i = 0; i < candidate_count && count < cap; i++) {
        int64_t group_id = candidates[i];
        if (!query || query(userdata, group_id))
            group_list_add(groups, &count, cap, group_id);
    }
    return count;
}

/// @brief Process one group bus and add the processed samples to the master bus.
/// @details The caller holds @p ctx->mutex for the full callback so
///          `vaud_set_group_effects_processor()` cannot replace the callback or
///          userdata while the render thread is using them.
/// @param ctx Context providing the preallocated normalized-float scratch bus.
/// @param process Installed in-place group processor.
/// @param userdata Opaque processor state.
/// @param group_id Logical group represented by @p group_accum.
/// @param group_accum Signed-PCM-unit input bus for the selected group.
/// @param master_accum Master bus that receives the processed contribution.
/// @param frames Number of stereo frames in both buses.
static void add_processed_group_to_master(vaud_context_t ctx,
                                          vaud_group_effects_process_fn process,
                                          void *userdata,
                                          int64_t group_id,
                                          int32_t *group_accum,
                                          int32_t *master_accum,
                                          int32_t frames) {
    if (!ctx || !process || !group_accum || !master_accum)
        return;

    size_t sample_count = (size_t)frames * (size_t)VAUD_CHANNELS;
    for (size_t i = 0; i < sample_count; i++)
        ctx->group_fx_buf[i] = (float)group_accum[i] / 32768.0f;

    process(userdata, group_id, ctx->group_fx_buf, frames, VAUD_CHANNELS, VAUD_SAMPLE_RATE);

    for (size_t i = 0; i < sample_count; i++) {
        float scaled = ctx->group_fx_buf[i] * 32768.0f;
        int32_t sample = vaud_mixer_float_to_accum_sample(scaled);
        master_accum[i] = vaud_mixer_saturating_add_i32(master_accum[i], sample);
    }
}

/// @brief Route active sources either directly to the master bus or through group effects.
/// @details Sources in effect-enabled groups are first mixed into the context's
///          reusable group accumulator, normalized for the callback, and then
///          saturating-added to @p accum. Other groups take the direct path.
///          The caller holds @p ctx->mutex throughout the operation.
/// @param ctx Context containing active sources and preallocated scratch buses.
/// @param accum Master signed-PCM-unit accumulator.
/// @param frames Number of stereo frames to mix.
/// @param master Sanitized context master-volume multiplier.
/// @param effect_groups Logical groups selected for callback processing.
/// @param effect_group_count Number of entries in @p effect_groups.
/// @param process Installed effects callback.
/// @param userdata Opaque callback state.
static void mix_with_group_effects(vaud_context_t ctx,
                                   int32_t *accum,
                                   int32_t frames,
                                   float master,
                                   const int64_t *effect_groups,
                                   int32_t effect_group_count,
                                   vaud_group_effects_process_fn process,
                                   void *userdata) {
    if (effect_group_count <= 0) {
        for (int32_t i = 0; i < VAUD_MAX_VOICES; i++)
            mix_voice(&ctx->voices[i],
                      accum,
                      frames,
                      master * mixer_group_duck_gain(ctx, ctx->voices[i].group_id));
        for (int32_t i = 0; i < ctx->music_count; i++) {
            if (ctx->active_music[i])
                mix_music(ctx->active_music[i],
                          accum,
                          frames,
                          master * mixer_group_duck_gain(ctx, ctx->active_music[i]->group_id));
        }
        return;
    }

    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        vaud_voice *voice = &ctx->voices[i];
        if (voice->state != VAUD_VOICE_PLAYING || !voice->sound)
            continue;
        if (group_list_contains(effect_groups, effect_group_count, voice->group_id))
            continue;
        mix_voice(voice, accum, frames, master * mixer_group_duck_gain(ctx, voice->group_id));
    }

    for (int32_t i = 0; i < ctx->music_count; i++) {
        vaud_music_t music = ctx->active_music[i];
        if (!music)
            continue;
        if (group_list_contains(effect_groups, effect_group_count, music->group_id))
            continue;
        mix_music(music, accum, frames, master * mixer_group_duck_gain(ctx, music->group_id));
    }

    size_t sample_count = (size_t)frames * (size_t)VAUD_CHANNELS;
    for (int32_t g = 0; g < effect_group_count; g++) {
        int64_t group_id = effect_groups[g];
        float group_master = master * mixer_group_duck_gain(ctx, group_id);
        memset(ctx->group_accum_buf, 0, sample_count * sizeof(int32_t));

        for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
            vaud_voice *voice = &ctx->voices[i];
            if (voice->state == VAUD_VOICE_PLAYING && voice->sound && voice->group_id == group_id) {
                mix_voice(voice, ctx->group_accum_buf, frames, group_master);
            }
        }

        for (int32_t i = 0; i < ctx->music_count; i++) {
            vaud_music_t music = ctx->active_music[i];
            if (music && music->group_id == group_id)
                mix_music(music, ctx->group_accum_buf, frames, group_master);
        }

        add_processed_group_to_master(
            ctx, process, userdata, group_id, ctx->group_accum_buf, accum, frames);
    }
}

//===----------------------------------------------------------------------===//
// Main Mixer Entry Point
//===----------------------------------------------------------------------===//

/// @copydoc vaud_mixer_render
void vaud_mixer_render(vaud_context_t ctx, int16_t *output, int32_t frames) {
    if (!ctx || !output || frames <= 0)
        return;
    vaud_stats_add(&ctx->stats.render_calls, 1);

    if (vaud_atomic_load_i32(&ctx->destroying) != 0) {
        vaud_zero_output(output, frames);
        return;
    }

    if (frames > VAUD_BUFFER_FRAMES) {
        int32_t offset = 0;
        while (offset < frames) {
            int32_t chunk = frames - offset;
            if (chunk > VAUD_BUFFER_FRAMES)
                chunk = VAUD_BUFFER_FRAMES;
            vaud_mixer_render(ctx, output + (size_t)offset * VAUD_CHANNELS, chunk);
            offset += chunk;
        }
        return;
    }

    /* H-1: Use pre-allocated 32-bit accumulator (no malloc in real-time audio callback).
     * ctx->accum_buf holds VAUD_BUFFER_FRAMES * VAUD_CHANNELS int32s. Oversized
     * backend periods are rendered above in bounded chunks. */
    int32_t *accum = ctx->accum_buf;

    /* Clear accumulator */
    size_t sample_count = (size_t)frames * (size_t)VAUD_CHANNELS;
    memset(accum, 0, sample_count * sizeof(int32_t));

    if (!vaud_mutex_trylock(&ctx->mutex)) {
        vaud_stats_add(&ctx->stats.mixer_lock_misses, 1);
        vaud_mixer_render_lock_miss_fallback(ctx, output, frames);
        return;
    }

    float master = vaud_mixer_unit_volume(ctx->master_volume);

    if (ctx->paused) {
        vaud_mutex_unlock(&ctx->mutex);
        vaud_zero_output(output, frames);
        return;
    }

    /* Advance sidechain duck envelopes once per chunk (before mixing so the
     * gains applied below reflect this chunk's trigger activity). */
    mixer_update_duck_rules(ctx, frames);

    int64_t candidate_groups[VAUD_MAX_VOICES + VAUD_MAX_MUSIC];
    int64_t effect_groups[VAUD_MAX_VOICES + VAUD_MAX_MUSIC] = {0};
    int32_t group_cap = (int32_t)(sizeof(effect_groups) / sizeof(effect_groups[0]));
    int32_t effect_group_count = 0;
    vaud_group_effects_query_fn query = ctx->group_effects_query;
    vaud_group_effects_process_fn process = ctx->group_effects_process;
    void *effects_userdata = ctx->group_effects_userdata;

    if (process) {
        int32_t candidate_count =
            collect_effect_group_candidates_locked(ctx, candidate_groups, group_cap);
        effect_group_count = filter_effect_groups_locked(
            query, effects_userdata, candidate_groups, candidate_count, effect_groups, group_cap);
        if (ctx->paused || vaud_atomic_load_i32(&ctx->destroying) != 0) {
            vaud_mutex_unlock(&ctx->mutex);
            vaud_zero_output(output, frames);
            return;
        }
    }

    mix_with_group_effects(
        ctx, accum, frames, master, effect_groups, effect_group_count, process, effects_userdata);

    if (ctx->frame_counter > INT64_MAX - (int64_t)frames)
        ctx->frame_counter = INT64_MAX;
    else
        ctx->frame_counter += frames;

    vaud_mutex_unlock(&ctx->mutex);

    /* Convert to 16-bit with soft clipping */
    for (size_t i = 0; i < sample_count; i++) {
        output[i] = soft_clip(accum[i]);
    }
    vaud_mixer_cache_output(ctx, output, frames);
    /* H-1: accum is ctx->accum_buf — no free needed */
}

/// @brief Render PCM for a platform device while honoring the silent-output policy.
/// @details Mixing still runs normally so voices, music positions, fades, effects,
///          telemetry, and lifecycle assertions advance exactly as they do in a
///          live application. Only the final device-bound samples are discarded.
/// @param ctx Audio context to advance.
/// @param output Device-bound interleaved signed-16 PCM buffer.
/// @param frames Number of stereo frames to render.
void vaud_mixer_render_device(vaud_context_t ctx, int16_t *output, int32_t frames) {
    vaud_mixer_render(ctx, output, frames);
    if (ctx && output && frames > 0 && ctx->device_output_silent)
        vaud_zero_output(output, frames);
}

//===----------------------------------------------------------------------===//
// Voice Management
//===----------------------------------------------------------------------===//

/// @brief Generate a positive voice identifier not used by any active slot.
/// @details Wraps the signed counter back to one before overflow and performs a
///          bounded collision scan so stale public handles cannot alias a live
///          voice merely because the generator wrapped.
/// @param ctx Context whose voice slots and next-id counter are inspected.
/// @return Unique identifier, or VAUD_INVALID_VOICE if none is found in the
///         bounded search.
static vaud_voice_id vaud_next_voice_id(vaud_context_t ctx) {
    if (!ctx)
        return VAUD_INVALID_VOICE;

    for (int32_t attempts = 0; attempts < VAUD_MAX_VOICES + 2; attempts++) {
        int32_t candidate = ctx->next_voice_id;
        if (ctx->next_voice_id >= INT32_MAX || ctx->next_voice_id <= 0 ||
            ctx->next_voice_id == VAUD_INVALID_VOICE) {
            ctx->next_voice_id = 1;
        } else {
            ctx->next_voice_id++;
        }
        if (candidate <= 0 || candidate == VAUD_INVALID_VOICE)
            continue;

        int in_use = 0;
        for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
            if (ctx->voices[i].state != VAUD_VOICE_INACTIVE && ctx->voices[i].id == candidate) {
                in_use = 1;
                break;
            }
        }
        if (!in_use)
            return (vaud_voice_id)candidate;
    }

    return VAUD_INVALID_VOICE;
}

/// @copydoc vaud_alloc_voice
vaud_voice *vaud_alloc_voice(vaud_context_t ctx) {
    if (!ctx)
        return NULL;

    vaud_voice *oldest = NULL;
    int64_t oldest_time = ctx->frame_counter + 1;

    /* First pass: look for inactive voice */
    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        if (ctx->voices[i].state == VAUD_VOICE_INACTIVE) {
            ctx->voices[i].id = vaud_next_voice_id(ctx);
            if (ctx->voices[i].id == VAUD_INVALID_VOICE)
                return NULL;
            ctx->voices[i].start_time = ctx->frame_counter;
            return &ctx->voices[i];
        }

        /* Track oldest non-looping voice for stealing */
        if (!ctx->voices[i].loop && ctx->voices[i].start_time < oldest_time) {
            oldest = &ctx->voices[i];
            oldest_time = ctx->voices[i].start_time;
        }
    }

    /* Second pass: steal oldest non-looping voice */
    if (oldest) {
        oldest->state = VAUD_VOICE_INACTIVE;
        oldest->sound = NULL;
        oldest->id = vaud_next_voice_id(ctx);
        if (oldest->id == VAUD_INVALID_VOICE)
            return NULL;
        oldest->start_time = ctx->frame_counter;
        return oldest;
    }

    /* All voices are looping - steal absolute oldest */
    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        if (ctx->voices[i].start_time < oldest_time) {
            oldest = &ctx->voices[i];
            oldest_time = ctx->voices[i].start_time;
        }
    }

    if (oldest) {
        oldest->state = VAUD_VOICE_INACTIVE;
        oldest->sound = NULL;
        oldest->id = vaud_next_voice_id(ctx);
        if (oldest->id == VAUD_INVALID_VOICE)
            return NULL;
        oldest->start_time = ctx->frame_counter;
        return oldest;
    }

    return NULL;
}

/// @copydoc vaud_find_voice
vaud_voice *vaud_find_voice(vaud_context_t ctx, vaud_voice_id id) {
    if (!ctx || id == VAUD_INVALID_VOICE)
        return NULL;

    for (int32_t i = 0; i < VAUD_MAX_VOICES; i++) {
        if (ctx->voices[i].id == id && ctx->voices[i].state != VAUD_VOICE_INACTIVE) {
            return &ctx->voices[i];
        }
    }

    return NULL;
}
