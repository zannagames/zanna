//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTAudioFxTests.cpp
// Purpose: Offline tests for runtime audio mix-group effects, parameter
//          sanitation, and invalid-group ownership paths.
// Key invariants:
//   - Filters, delays, and reverbs produce finite deterministic output.
//   - Invalid groups fail before retaining any effect or delay-line state.
//   - Bypass, removal, and clearing restore identity processing.
// Ownership/Lifetime:
//   - Every valid effect chain is cleared before a test returns; invalid add
//     calls retain no process-global state.
// Links: src/runtime/audio/rt_audio_fx.c, src/runtime/audio/rt_audio_fx.h
//
//===----------------------------------------------------------------------===//

#include "rt_audio_fx.h"
#include "rt_mixgroup.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 2;
constexpr double kPi = 3.14159265358979323846;

double rms(const std::vector<float> &samples) {
    double sum = 0.0;
    for (float sample : samples)
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

void fill_sine_pair(std::vector<float> &samples, double freq_a, double freq_b) {
    for (size_t frame = 0; frame < samples.size() / kChannels; frame++) {
        double t = static_cast<double>(frame) / static_cast<double>(kRate);
        float sample = static_cast<float>(0.45 * std::sin(2.0 * kPi * freq_a * t) +
                                          0.45 * std::sin(2.0 * kPi * freq_b * t));
        samples[frame * kChannels] = sample;
        samples[frame * kChannels + 1] = sample;
    }
}

void assert_identity_after_process(int64_t group) {
    std::vector<float> original(256 * kChannels);
    fill_sine_pair(original, 330.0, 1200.0);
    std::vector<float> processed = original;
    rt_audio_fx_process_group(group, processed.data(), 256, kChannels, static_cast<int32_t>(kRate));
    for (size_t i = 0; i < original.size(); i++)
        assert(std::fabs(original[i] - processed[i]) < 1.0e-6f);
}

void test_lowpass_attenuates_high_frequency_mix() {
    rt_audio_fx_clear_all();
    std::vector<float> samples(4096 * kChannels);
    fill_sine_pair(samples, 1000.0, 8000.0);
    double before = rms(samples);

    int64_t fx = rt_audio_fx_add_lowpass(RT_MIXGROUP_SFX, 2000.0, 0.707);
    assert(fx > 0);
    rt_audio_fx_process_group(
        RT_MIXGROUP_SFX, samples.data(), 4096, kChannels, static_cast<int32_t>(kRate));
    double after = rms(samples);
    assert(after < before * 0.80);
    rt_audio_fx_clear_all();
}

void test_delay_impulse_lands_at_configured_offset() {
    rt_audio_fx_clear_all();
    constexpr double delay_ms = 10.0;
    constexpr int delay_frames = static_cast<int>(delay_ms / 1000.0 * kRate + 0.5);
    std::vector<float> samples((delay_frames + 8) * kChannels, 0.0f);
    samples[0] = 1.0f;
    samples[1] = 1.0f;

    int64_t fx = rt_audio_fx_add_delay(RT_MIXGROUP_SFX, delay_ms, 0.0, 1.0);
    assert(fx > 0);
    rt_audio_fx_process_group(RT_MIXGROUP_SFX,
                              samples.data(),
                              static_cast<int32_t>(samples.size() / kChannels),
                              kChannels,
                              static_cast<int32_t>(kRate));
    assert(std::fabs(samples[0]) < 1.0e-6f);
    assert(std::fabs(samples[static_cast<size_t>(delay_frames) * kChannels] - 1.0f) < 1.0e-6f);
    rt_audio_fx_clear_all();
}

void test_reverb_generates_decaying_tail() {
    rt_audio_fx_clear_all();
    constexpr int frames = 12000;
    std::vector<float> samples(frames * kChannels, 0.0f);
    samples[0] = 1.0f;
    samples[1] = 1.0f;

    int64_t fx = rt_audio_fx_add_reverb(RT_MIXGROUP_SFX, 0.8, 0.35, 1.0);
    assert(fx > 0);
    rt_audio_fx_process_group(
        RT_MIXGROUP_SFX, samples.data(), frames, kChannels, static_cast<int32_t>(kRate));

    double mid = 0.0;
    double final = 0.0;
    for (int i = 3000; i < 6000; i++)
        mid += std::fabs(samples[static_cast<size_t>(i) * kChannels]);
    for (int i = 11000; i < frames; i++)
        final += std::fabs(samples[static_cast<size_t>(i) * kChannels]);
    mid /= 3000.0;
    final /= 1000.0;
    assert(mid > 1.0e-4);
    assert(final > 0.0);
    assert(final < mid);
    rt_audio_fx_clear_all();
}

void test_bypass_remove_clear_are_identity() {
    rt_audio_fx_clear_all();
    int64_t fx = rt_audio_fx_add_lowpass(RT_MIXGROUP_SFX, 400.0, 0.707);
    assert(fx > 0);
    rt_audio_fx_set_bypass(RT_MIXGROUP_SFX, fx, 1);
    assert_identity_after_process(RT_MIXGROUP_SFX);

    rt_audio_fx_set_bypass(RT_MIXGROUP_SFX, fx, 0);
    rt_audio_fx_remove(RT_MIXGROUP_SFX, fx);
    assert_identity_after_process(RT_MIXGROUP_SFX);

    fx = rt_audio_fx_add_lowpass(RT_MIXGROUP_SFX, 400.0, 0.707);
    assert(fx > 0);
    rt_audio_fx_clear_group(RT_MIXGROUP_SFX);
    assert_identity_after_process(RT_MIXGROUP_SFX);
    rt_audio_fx_clear_all();
}

void test_invalid_groups_reject_every_effect_kind() {
    rt_audio_fx_clear_all();
    constexpr int64_t invalid_groups[] = {-1, RT_MIXGROUP_MAX_GROUPS};
    for (int64_t group : invalid_groups) {
        assert(rt_audio_fx_add_lowpass(group, 1000.0, 0.707) == -1);
        assert(rt_audio_fx_add_highpass(group, 1000.0, 0.707) == -1);
        assert(rt_audio_fx_add_peaking(group, 1000.0, 0.707, 3.0) == -1);
        assert(rt_audio_fx_add_delay(group, 250.0, 0.5, 0.5) == -1);
        assert(rt_audio_fx_add_reverb(group, 0.5, 0.5, 0.5) == -1);
        assert(rt_audio_fx_group_has_effects(group) == 0);
    }
    rt_audio_fx_clear_all();
}

} // namespace

void test_peaking_sanitizes_gain() {
    // VDOC-122: non-finite or extreme gain must not poison the biquad —
    // output stays finite and non-silent for ordinary signal.
    rt_audio_fx_clear_all();
    const double kNan = std::numeric_limits<double>::quiet_NaN();
    int64_t fx = rt_audio_fx_add_peaking(RT_MIXGROUP_SFX, 1000.0, 0.707, kNan);
    (void)fx;

    std::vector<float> samples(1024 * kChannels);
    fill_sine_pair(samples, 440.0, 2000.0);
    rt_audio_fx_process_group(
        RT_MIXGROUP_SFX, samples.data(), 1024, kChannels, static_cast<int32_t>(kRate));

    bool all_finite = true;
    for (float v : samples)
        all_finite = all_finite && std::isfinite(v);
    assert(all_finite);
    assert(rms(samples) > 0.01); // not silenced

    rt_audio_fx_clear_all();

    // Extreme finite gain clamps instead of exploding.
    rt_audio_fx_add_peaking(RT_MIXGROUP_SFX, 1000.0, 0.707, 1.0e9);
    std::vector<float> loud(1024 * kChannels);
    fill_sine_pair(loud, 440.0, 2000.0);
    rt_audio_fx_process_group(
        RT_MIXGROUP_SFX, loud.data(), 1024, kChannels, static_cast<int32_t>(kRate));
    for (float v : loud)
        assert(std::isfinite(v));
    rt_audio_fx_clear_all();
}

int main() {
    test_peaking_sanitizes_gain();
    test_lowpass_attenuates_high_frequency_mix();
    test_delay_impulse_lands_at_configured_offset();
    test_reverb_generates_decaying_tail();
    test_bypass_remove_clear_are_identity();
    test_invalid_groups_reject_every_effect_kind();
    std::printf("Audio FX tests passed.\n");
    return 0;
}
