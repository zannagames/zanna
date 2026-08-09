//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTSound3DTests.cpp
// Purpose: Test isolated Sound3D spatial audio contracts with local backend stubs.
// Key invariants:
//   - Spatial volume and pan math stays deterministic without a real audio backend.
//   - Extreme finite orientation and velocity inputs produce bounded, physical results.
//   - Voice table growth and invalid-input handling keep stable contract behavior.
// Ownership/Lifetime:
//   - Stub state is process-local and reset by each test that observes it.
//   - Test vectors are stack-owned for the duration of each runtime call.
// Links: src/runtime/audio/rt_sound3d.c
//
//===----------------------------------------------------------------------===//

#include "rt_sound3d.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

struct Vec3 {
    double x;
    double y;
    double z;
};

int64_t g_next_voice = 1;
int64_t g_next_play_result = 0;
int64_t g_last_play_volume = -1;
int64_t g_last_play_pan = 0;
int64_t g_last_update_voice = -1;
int64_t g_last_update_volume = -1;
int64_t g_last_update_pan = 0;
double g_last_update_pitch = 0.0;
bool g_voices_playing = true;

void reset_audio_stub_state() {
    g_next_play_result = 0;
    g_last_play_volume = -1;
    g_last_play_pan = 0;
    g_last_update_voice = -1;
    g_last_update_volume = -1;
    g_last_update_pan = 0;
    g_last_update_pitch = 0.0;
}

} // namespace

extern "C" double rt_vec3_x(void *v) {
    return static_cast<Vec3 *>(v)->x;
}

extern "C" double rt_vec3_y(void *v) {
    return static_cast<Vec3 *>(v)->y;
}

extern "C" double rt_vec3_z(void *v) {
    return static_cast<Vec3 *>(v)->z;
}

extern "C" int64_t rt_sound_play_ex(void *, int64_t volume, int64_t pan) {
    g_last_play_volume = volume;
    g_last_play_pan = pan;
    if (g_next_play_result != 0)
        return g_next_play_result;
    return g_next_voice++;
}

extern "C" void rt_voice_set_volume(int64_t voice, int64_t volume) {
    g_last_update_voice = voice;
    g_last_update_volume = volume;
}

extern "C" void rt_voice_set_pan(int64_t voice, int64_t pan) {
    g_last_update_voice = voice;
    g_last_update_pan = pan;
}

extern "C" void rt_voice_set_pitch(int64_t voice, double pitch) {
    g_last_update_voice = voice;
    g_last_update_pitch = pitch;
}

extern "C" int64_t rt_voice_is_playing(int64_t voice) {
    // Stub: report every valid voice as still playing so the voice table's
    // full-eviction path falls through to round-robin (its prior behavior)
    // instead of reclaiming a "finished" slot during these unit tests.
    return g_voices_playing && voice > 0 ? 1 : 0;
}

static void test_origin_has_full_volume_and_zero_pan() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{0.0, 0.0, 0.0};
    reset_audio_stub_state();

    rt_sound3d_set_listener(&listener, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 80);

    assert(voice > 0);
    assert(g_last_play_volume == 80);
    assert(g_last_play_pan == 0);
}

static void test_pan_and_attenuation_are_derived_from_position() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{5.0, 0.0, 0.0};
    reset_audio_stub_state();

    rt_sound3d_set_listener(&listener, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 100);

    assert(voice > 0);
    assert(g_last_play_volume == 50);
    assert(g_last_play_pan == 100);
}

static void test_update_voice_reuses_original_base_volume_and_distance() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{5.0, 0.0, 0.0};
    reset_audio_stub_state();

    rt_sound3d_set_listener(&listener, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 40);
    assert(voice > 0);
    assert(g_last_play_volume == 20);

    reset_audio_stub_state();
    rt_sound3d_update_voice(voice, &source, 0.0);

    assert(g_last_update_voice == voice);
    assert(g_last_update_volume == 20);
    assert(g_last_update_pan == 100);
    /* A stationary source relative to a stationary listener is Doppler-neutral. */
    assert(g_last_update_pitch == 1.0);
}

static void test_update_voice_persists_distance_overrides() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{12.0, 0.0, 0.0};
    reset_audio_stub_state();
    rt_sound3d_set_listener(&listener, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 40);
    assert(voice > 0);

    reset_audio_stub_state();
    rt_sound3d_update_voice_ex(voice, &source, nullptr, 4.0, 20.0);
    assert(g_last_update_volume == 20);

    reset_audio_stub_state();
    rt_sound3d_update_voice_ex(voice, &source, nullptr, 0.0, 0.0);
    assert(g_last_update_volume == 20);
}

static void test_update_voice_ignores_finished_voice_ids() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{5.0, 0.0, 0.0};
    reset_audio_stub_state();
    rt_sound3d_set_listener(&listener, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 40);
    assert(voice > 0);

    reset_audio_stub_state();
    g_voices_playing = false;
    rt_sound3d_update_voice(voice, &source, 0.0);
    assert(g_last_update_voice == -1);
    g_voices_playing = true;
}

static void test_invalid_inputs_are_ignored() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    reset_audio_stub_state();

    rt_sound3d_set_listener(&listener, &forward);
    assert(rt_sound3d_play_at(nullptr, &listener, 10.0, 100) == -1);
    assert(rt_sound3d_play_at(reinterpret_cast<void *>(1), nullptr, 10.0, 100) == -1);
    rt_sound3d_update_voice(0, &listener, 5.0);
    assert(g_last_update_voice == -1);
}

static void test_play_at_reports_backend_failure_with_minus_one() {
    // VDOC-120: spatial play uses the same -1 failure sentinel as Sound.Play*.
    Vec3 source{0.0, 0.0, 0.0};
    reset_audio_stub_state();
    g_next_play_result = -1;

    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 100);

    assert(voice == -1);
}

static void test_set_listener_accepts_partial_null_inputs() {
    Vec3 forward{1.0, 0.0, 0.0};
    Vec3 source{0.0, 0.0, 5.0};
    reset_audio_stub_state();

    rt_sound3d_set_listener(nullptr, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 100);

    assert(voice > 0);
    assert(g_last_play_pan == 100);
}

static void test_nonfinite_positions_do_not_escape_clamps() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{NAN, INFINITY, -INFINITY};
    reset_audio_stub_state();

    rt_sound3d_set_listener(&listener, &forward);
    int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, NAN, 1000);

    assert(voice > 0);
    assert(g_last_play_volume >= 0 && g_last_play_volume <= 100);
    assert(g_last_play_pan >= -100 && g_last_play_pan <= 100);
}

static void test_extreme_finite_basis_preserves_direction() {
    rt_sound3d_listener_state listener;
    const double huge = std::numeric_limits<double>::max();
    double position[3] = {0.0, 0.0, 0.0};
    double forward[3] = {huge, huge, 0.0};
    double up[3] = {0.0, 0.0, huge};

    rt_sound3d_listener_state_set_pose(&listener, position, forward, up, nullptr);

    assert(std::isfinite(listener.forward[0]));
    assert(std::isfinite(listener.forward[1]));
    assert(std::fabs(listener.forward[0] - std::sqrt(0.5)) < 1e-12);
    assert(std::fabs(listener.forward[1] - std::sqrt(0.5)) < 1e-12);
    assert(std::fabs(listener.up[2] - 1.0) < 1e-12);
}

static void test_active_listener_snapshot_is_sanitized() {
    rt_sound3d_listener_state listener;
    rt_sound3d_listener_state effective;
    const double huge = std::numeric_limits<double>::max();
    listener.position[0] = std::numeric_limits<double>::infinity();
    listener.position[1] = -std::numeric_limits<double>::infinity();
    listener.position[2] = std::numeric_limits<double>::quiet_NaN();
    listener.forward[0] = huge;
    listener.forward[1] = huge;
    listener.forward[2] = 0.0;
    listener.right[0] = std::numeric_limits<double>::quiet_NaN();
    listener.right[1] = std::numeric_limits<double>::infinity();
    listener.right[2] = 0.0;
    listener.up[0] = 0.0;
    listener.up[1] = 0.0;
    listener.up[2] = huge;
    listener.velocity[0] = huge;
    listener.velocity[1] = -huge;
    listener.velocity[2] = std::numeric_limits<double>::quiet_NaN();
    listener.valid = -7;

    rt_sound3d_set_active_listener_state(&listener);
    rt_sound3d_get_effective_listener_state(&effective);

    assert(effective.valid == 1);
    for (int lane = 0; lane < 3; ++lane) {
        assert(std::isfinite(effective.position[lane]));
        assert(std::isfinite(effective.forward[lane]));
        assert(std::isfinite(effective.right[lane]));
        assert(std::isfinite(effective.up[lane]));
        assert(std::isfinite(effective.velocity[lane]));
        assert(std::fabs(effective.velocity[lane]) <= 1000000.0);
    }
    assert(std::fabs(effective.forward[0] - std::sqrt(0.5)) < 1e-12);
    assert(std::fabs(effective.forward[1] - std::sqrt(0.5)) < 1e-12);
    rt_sound3d_clear_active_listener_state();
}

static void test_compute_fails_closed_and_repairs_listener_basis() {
    rt_sound3d_listener_state invalid_listener;
    rt_sound3d_listener_state corrupt_listener;
    double fallback_position[3] = {0.0, 0.0, 0.0};
    double fallback_forward[3] = {0.0, 0.0, -1.0};
    double ignored_position[3] = {100.0, 0.0, 0.0};
    double source[3] = {5.0, 0.0, 0.0};
    int64_t volume = 77;
    int64_t pan = 88;

    rt_sound3d_listener_state_set(&invalid_listener, ignored_position, fallback_forward, nullptr);
    invalid_listener.valid = 0;
    rt_sound3d_set_listener(&fallback_position, &fallback_forward);
    rt_sound3d_compute_voice_params(&invalid_listener, fallback_position, 10.0, 100, &volume, &pan);
    assert(volume == 100);
    assert(pan == 0);

    rt_sound3d_compute_voice_params(nullptr, nullptr, 10.0, 100, &volume, &pan);
    assert(volume == 0);
    assert(pan == 0);

    rt_sound3d_listener_state_identity(&corrupt_listener);
    corrupt_listener.right[0] = 0.0;
    corrupt_listener.right[1] = 0.0;
    corrupt_listener.right[2] = 0.0;
    volume = 0;
    pan = 0;
    rt_sound3d_compute_voice_params(&corrupt_listener, source, 10.0, 100, &volume, &pan);
    assert(volume == 50);
    assert(pan == 100);
}

static void test_doppler_singularities_clamp_in_physical_direction() {
    rt_sound3d_listener_state listener;
    double listener_position[3] = {0.0, 0.0, 0.0};
    double forward[3] = {0.0, 0.0, -1.0};
    double source_position[3] = {10.0, 0.0, 0.0};
    double sonic_approach[3] = {-343.0, 0.0, 0.0};
    double supersonic_approach[3] = {-1000.0, 0.0, 0.0};
    int64_t volume = 0;
    int64_t pan = 0;
    double doppler = 1.0;

    rt_sound3d_listener_state_set(&listener, listener_position, forward, nullptr);
    rt_sound3d_compute_voice_params_ex(
        &listener, source_position, sonic_approach, 1.0, 50.0, 100, &volume, &pan, &doppler);
    assert(doppler == 2.0);

    rt_sound3d_compute_voice_params_ex(
        &listener, source_position, supersonic_approach, 1.0, 50.0, 100, &volume, &pan, &doppler);
    assert(doppler == 2.0);

    listener.velocity[0] = -343.0;
    rt_sound3d_compute_voice_params_ex(
        &listener, source_position, nullptr, 1.0, 50.0, 100, &volume, &pan, &doppler);
    assert(doppler == 0.5);
}

static void test_voice_tracking_overwrites_as_ring() {
    Vec3 listener{0.0, 0.0, 0.0};
    Vec3 forward{0.0, 0.0, -1.0};
    Vec3 source{5.0, 0.0, 0.0};
    reset_audio_stub_state();
    rt_sound3d_set_listener(&listener, &forward);

    int64_t tracked_after_wrap = 0;
    for (int i = 0; i < 66; i++) {
        int64_t voice = rt_sound3d_play_at(reinterpret_cast<void *>(1), &source, 10.0, 40);
        assert(voice > 0);
        if (i == 64)
            tracked_after_wrap = voice;
    }

    reset_audio_stub_state();
    rt_sound3d_update_voice(tracked_after_wrap, &source, 0.0);

    assert(g_last_update_voice == tracked_after_wrap);
    assert(g_last_update_volume == 20);
    assert(g_last_update_pan == 100);
}

int main() {
    test_origin_has_full_volume_and_zero_pan();
    test_pan_and_attenuation_are_derived_from_position();
    test_update_voice_reuses_original_base_volume_and_distance();
    test_update_voice_persists_distance_overrides();
    test_update_voice_ignores_finished_voice_ids();
    test_invalid_inputs_are_ignored();
    test_play_at_reports_backend_failure_with_minus_one();
    test_set_listener_accepts_partial_null_inputs();
    test_nonfinite_positions_do_not_escape_clamps();
    test_extreme_finite_basis_preserves_direction();
    test_active_listener_snapshot_is_sanitized();
    test_compute_fails_closed_and_repairs_listener_basis();
    test_doppler_singularities_clamp_in_physical_direction();
    test_voice_tracking_overwrites_as_ring();
    std::printf("RTSound3DTests passed.\n");
    return 0;
}
