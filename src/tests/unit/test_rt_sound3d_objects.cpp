//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_sound3d_objects.cpp
// Purpose: Unit tests for SoundListener3D / SoundSource3D bindings and basic
//   playback lifecycle.
// Key invariants:
//   - Object access repairs retained numeric state before it reaches the mixer.
//   - Bound transforms and active-listener selection remain deterministic.
// Ownership/Lifetime:
//   - Runtime objects are GC-managed by the test runtime for process lifetime.
//   - Private-layout views are borrowed only for adversarial state injection.
// Links: src/runtime/audio/rt_sound3d_objects.c, src/runtime/audio/rt_sound3d.c
//
//===----------------------------------------------------------------------===//

#include "rt_audio.h"
#include "rt_canvas3d.h"
#include "rt_mixgroup.h"
#include "rt_scene3d.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"

#include <cmath>
#include <cstdio>
#include <limits>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_synth_tone(int64_t hz, int64_t duration_ms, int64_t waveform);
}

struct SoundListener3DTestLayout {
    void *vptr;
    rt_sound3d_listener_state state;
    void *bound_node;
    void *bound_camera;
    double last_sync_position[3];
    int8_t has_last_sync_position;
    int8_t is_active;
    void *prev;
    void *next;
};

struct SoundSource3DTestLayout {
    void *vptr;
    void *sound;
    void *bound_node;
    double position[3];
    double velocity[3];
    double doppler_factor;
    double last_sync_position[3];
    int8_t has_last_sync_position;
    double ref_distance;
    double max_distance;
    int64_t volume;
    int64_t voice_id;
    int8_t looping;
    double pitch;
    double occlusion;
    int64_t mix_group;
    void *prev;
    void *next;
};

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond))                                                                               \
            std::fprintf(stderr, "FAIL: %s\n", msg);                                               \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (std::fabs((double)(a) - (double)(b)) > (eps))                                          \
            std::fprintf(                                                                          \
                stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));        \
        else                                                                                       \
            tests_passed++;                                                                        \
    } while (0)

static void test_listener_follows_bound_camera() {
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *listener = rt_soundlistener3d_new();
    void *pos;
    void *forward;
    void *up;
    void *vel;

    rt_camera3d_look_at(
        camera, rt_vec3_new(1.0, 2.0, 6.0), rt_vec3_new(1.0, 2.0, 5.0), rt_vec3_new(0.0, 1.0, 0.0));
    rt_soundlistener3d_bind_camera(listener, camera);
    rt_soundlistener3d_set_is_active(listener, 1);
    rt_sound3d_sync_bindings(0.25);

    pos = rt_soundlistener3d_get_position(listener);
    forward = rt_soundlistener3d_get_forward(listener);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.001, "SoundListener3D follows camera X");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.001, "SoundListener3D follows camera Y");
    EXPECT_NEAR(rt_vec3_z(pos), 6.0, 0.001, "SoundListener3D follows camera Z");
    EXPECT_NEAR(rt_vec3_x(forward), 0.0, 0.001, "SoundListener3D derives camera forward X");
    EXPECT_NEAR(rt_vec3_y(forward), 0.0, 0.001, "SoundListener3D derives camera forward Y");
    EXPECT_NEAR(rt_vec3_z(forward), -1.0, 0.001, "SoundListener3D derives camera forward Z");

    rt_camera3d_look_at(
        camera, rt_vec3_new(3.0, 2.0, 6.0), rt_vec3_new(3.0, 2.0, 5.0), rt_vec3_new(0.0, 1.0, 0.0));
    rt_sound3d_sync_bindings(0.5);
    vel = rt_soundlistener3d_get_velocity(listener);
    EXPECT_NEAR(
        rt_vec3_x(vel), 4.0, 0.05, "SoundListener3D computes velocity from bound camera motion");
    EXPECT_NEAR(rt_vec3_z(vel), 0.0, 0.05, "SoundListener3D velocity stays flat on unchanged Z");

    rt_camera3d_look_at(
        camera, rt_vec3_new(7.0, 2.0, 6.0), rt_vec3_new(7.0, 2.0, 5.0), rt_vec3_new(0.0, 1.0, 0.0));
    pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(
        rt_vec3_x(pos), 7.0, 0.001, "SoundListener3D getter refreshes a moved bound camera");
    rt_sound3d_sync_bindings(0.5);
    vel = rt_soundlistener3d_get_velocity(listener);
    EXPECT_NEAR(
        rt_vec3_x(vel), 8.0, 0.05, "zero-delta binding refresh preserves the velocity baseline");

    rt_camera3d_look_at(
        camera, rt_vec3_new(7.0, 2.0, 6.0), rt_vec3_new(7.0, 2.0, 5.0), rt_vec3_new(1.0, 0.0, 0.0));
    rt_sound3d_sync_bindings(0.25);
    up = rt_soundlistener3d_get_up(listener);
    EXPECT_NEAR(rt_vec3_x(up), 1.0, 0.001, "SoundListener3D follows bound camera roll X");
    EXPECT_NEAR(rt_vec3_y(up), 0.0, 0.001, "SoundListener3D follows bound camera roll Y");
    EXPECT_NEAR(rt_vec3_z(up), 0.0, 0.001, "SoundListener3D follows bound camera roll Z");
}

static void test_source_follows_bound_node_in_world_space() {
    void *scene = rt_scene3d_new();
    void *parent = rt_scene_node3d_new();
    void *child = rt_scene_node3d_new();
    void *source = rt_soundsource3d_new(NULL);
    void *pos;
    void *vel;

    rt_scene_node3d_set_position(parent, 2.0, 0.0, 3.0);
    rt_scene_node3d_set_position(child, -1.0, 0.5, 4.0);
    rt_scene_node3d_add_child(parent, child);
    rt_scene3d_add(scene, parent);

    rt_soundsource3d_bind_node(source, child);
    rt_scene3d_sync_bindings(scene, 0.25);
    pos = rt_soundsource3d_get_position(source);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.001, "SoundSource3D follows bound node world X");
    EXPECT_NEAR(rt_vec3_y(pos), 0.5, 0.001, "SoundSource3D follows bound node world Y");
    EXPECT_NEAR(rt_vec3_z(pos), 7.0, 0.001, "SoundSource3D follows bound node world Z");
    double raw_x = 0.0;
    double raw_y = 0.0;
    double raw_z = 0.0;
    EXPECT_TRUE(rt_soundsource3d_get_position_components(source, &raw_x, &raw_y, &raw_z) != 0,
                "SoundSource3D exposes allocation-free position components");
    EXPECT_NEAR(raw_x, 1.0, 0.001, "raw SoundSource3D position preserves world X");
    EXPECT_NEAR(raw_y, 0.5, 0.001, "raw SoundSource3D position preserves world Y");
    EXPECT_NEAR(raw_z, 7.0, 0.001, "raw SoundSource3D position preserves world Z");

    rt_scene_node3d_set_position(parent, 4.0, 0.0, 4.0);
    rt_scene3d_sync_bindings(scene, 0.5);
    pos = rt_soundsource3d_get_position(source);
    vel = rt_soundsource3d_get_velocity(source);
    EXPECT_NEAR(
        rt_vec3_x(pos), 3.0, 0.001, "SoundSource3D updates cached world X after parent motion");
    EXPECT_NEAR(
        rt_vec3_z(pos), 8.0, 0.001, "SoundSource3D updates cached world Z after parent motion");
    EXPECT_NEAR(rt_vec3_x(vel), 4.0, 0.05, "SoundSource3D computes bound-node X velocity");
    EXPECT_NEAR(rt_vec3_z(vel), 2.0, 0.05, "SoundSource3D computes bound-node Z velocity");
}

static void test_bound_source_manual_velocity_survives_one_sync() {
    void *node = rt_scene_node3d_new();
    void *source = rt_soundsource3d_new(nullptr);
    void *velocity;

    rt_soundsource3d_bind_node(source, node);
    rt_soundsource3d_set_velocity(source, rt_vec3_new(5.0, 0.0, 0.0));
    rt_scene_node3d_set_position(node, 2.0, 0.0, 0.0);
    rt_sound3d_sync_bindings(0.5);
    velocity = rt_soundsource3d_get_velocity(source);
    EXPECT_NEAR(rt_vec3_x(velocity),
                5.0,
                0.001,
                "manual source velocity survives the promised next bound sync");

    rt_scene_node3d_set_position(node, 4.0, 0.0, 0.0);
    rt_sound3d_sync_bindings(0.5);
    velocity = rt_soundsource3d_get_velocity(source);
    EXPECT_NEAR(
        rt_vec3_x(velocity), 4.0, 0.05, "bound source resumes derived velocity after one sync");
}

static void test_listener_bound_node_direction_ignores_large_translation() {
    void *node = rt_scene_node3d_new();
    void *listener = rt_soundlistener3d_new();
    void *forward;
    void *up;
    const double half_sqrt_two = std::sqrt(0.5);

    rt_scene_node3d_set_transform(node,
                                  1000000000000.0,
                                  -1000000000000.0,
                                  1000000000000.0,
                                  0.0,
                                  -half_sqrt_two,
                                  0.0,
                                  half_sqrt_two,
                                  3.0,
                                  2.0,
                                  4.0);
    rt_soundlistener3d_bind_node(listener, node);
    rt_sound3d_sync_bindings(0.25);

    forward = rt_soundlistener3d_get_forward(listener);
    up = rt_soundlistener3d_get_up(listener);
    EXPECT_NEAR(rt_vec3_x(forward),
                1.0,
                0.001,
                "SoundListener3D node forward ignores large world translation");
    EXPECT_NEAR(
        rt_vec3_y(forward), 0.0, 0.001, "SoundListener3D node forward remains normalized on Y");
    EXPECT_NEAR(
        rt_vec3_z(forward), 0.0, 0.001, "SoundListener3D node forward applies world rotation");
    EXPECT_NEAR(rt_vec3_x(up),
                0.0,
                0.001,
                "SoundListener3D node up remains orthogonal after nonuniform scale");
    EXPECT_NEAR(rt_vec3_y(up),
                1.0,
                0.001,
                "SoundListener3D node up remains normalized after nonuniform scale");
    EXPECT_NEAR(rt_vec3_z(up), 0.0, 0.001, "SoundListener3D node up applies world orientation");
}

static void test_invalid_audio_handles_are_ignored() {
    void *camera = rt_camera3d_new(60.0, 1.0, 0.1, 100.0);
    void *node = rt_scene_node3d_new();
    void *listener = rt_soundlistener3d_new();
    void *source = rt_soundsource3d_new(NULL);
    void *pos;

    rt_soundsource3d_set_position(source, rt_vec3_new(2.0, 3.0, 4.0));
    rt_soundsource3d_set_position(source, listener);
    rt_soundsource3d_set_position(source, nullptr);
    pos = rt_soundsource3d_get_position(source);
    EXPECT_NEAR(rt_vec3_x(pos), 2.0, 0.001, "SoundSource3D ignores non-Vec3 position handles");
    EXPECT_NEAR(rt_vec3_y(pos), 3.0, 0.001, "SoundSource3D keeps Y after bad position handle");

    rt_soundsource3d_set_velocity(source, rt_vec3_new(6.0, 7.0, 8.0));
    rt_soundsource3d_set_velocity(source, nullptr);
    void *velocity = rt_soundsource3d_get_velocity(source);
    EXPECT_NEAR(rt_vec3_x(velocity), 6.0, 0.001, "SoundSource3D ignores null velocity handles");

    rt_soundlistener3d_set_position(listener, rt_vec3_new(4.0, 5.0, 6.0));
    rt_soundlistener3d_set_position(listener, nullptr);
    pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(rt_vec3_x(pos), 4.0, 0.001, "SoundListener3D ignores null position handles");
    rt_soundlistener3d_set_forward(listener, rt_vec3_new(1.0, 0.0, 0.0));
    rt_soundlistener3d_set_forward(listener, nullptr);
    void *direction = rt_soundlistener3d_get_forward(listener);
    EXPECT_NEAR(rt_vec3_x(direction), 1.0, 0.001, "SoundListener3D ignores null forward handles");
    rt_soundlistener3d_set_up(listener, rt_vec3_new(0.0, 0.0, 1.0));
    rt_soundlistener3d_set_up(listener, nullptr);
    direction = rt_soundlistener3d_get_up(listener);
    EXPECT_NEAR(rt_vec3_z(direction), 1.0, 0.001, "SoundListener3D ignores null up handles");
    rt_soundlistener3d_set_velocity(listener, rt_vec3_new(9.0, 8.0, 7.0));
    rt_soundlistener3d_set_velocity(listener, nullptr);
    velocity = rt_soundlistener3d_get_velocity(listener);
    EXPECT_NEAR(rt_vec3_x(velocity), 9.0, 0.001, "SoundListener3D ignores null velocity handles");

    rt_scene_node3d_set_position(node, 5.0, 0.0, 0.0);
    rt_soundsource3d_bind_node(source, node);
    rt_sound3d_sync_bindings(0.25);
    rt_soundsource3d_bind_node(source, listener);
    rt_scene_node3d_set_position(node, 7.0, 0.0, 0.0);
    rt_sound3d_sync_bindings(0.25);
    pos = rt_soundsource3d_get_position(source);
    EXPECT_NEAR(rt_vec3_x(pos),
                7.0,
                0.001,
                "SoundSource3D rejects bad node bind without clearing old bind");

    rt_camera3d_look_at(
        camera, rt_vec3_new(1.0, 0.0, 4.0), rt_vec3_new(1.0, 0.0, 3.0), rt_vec3_new(0.0, 1.0, 0.0));
    rt_soundlistener3d_bind_camera(listener, camera);
    rt_sound3d_sync_bindings(0.25);
    rt_soundlistener3d_bind_camera(listener, source);
    rt_camera3d_look_at(
        camera, rt_vec3_new(3.0, 0.0, 4.0), rt_vec3_new(3.0, 0.0, 3.0), rt_vec3_new(0.0, 1.0, 0.0));
    rt_sound3d_sync_bindings(0.25);
    pos = rt_soundlistener3d_get_position(listener);
    EXPECT_NEAR(rt_vec3_x(pos),
                3.0,
                0.001,
                "SoundListener3D rejects bad camera bind without clearing old bind");

    rt_soundlistener3d_set_position(source, rt_vec3_new(9.0, 0.0, 0.0));
    EXPECT_TRUE(rt_soundlistener3d_get_is_active(source) == 0,
                "SoundListener3D accessors reject source handles");
    rt_soundsource3d_set_max_distance(source, INFINITY);
    EXPECT_NEAR(rt_soundsource3d_get_max_distance(source),
                0.0,
                0.001,
                "SoundSource3D rejects infinite max distance");
    rt_soundsource3d_set_ref_distance(source, 12.0);
    rt_soundsource3d_set_max_distance(source, 4.0);
    EXPECT_NEAR(rt_soundsource3d_get_ref_distance(source),
                12.0,
                0.001,
                "SoundSource3D stores reference distance");
    EXPECT_NEAR(rt_soundsource3d_get_max_distance(source),
                12.0,
                0.001,
                "SoundSource3D raises max distance to reference distance");
}

static void test_reference_distance_and_doppler_math() {
    rt_sound3d_listener_state listener;
    double listener_pos[3] = {0.0, 0.0, 0.0};
    double forward[3] = {0.0, 0.0, -1.0};
    double tilted_up[3] = {1.0, 0.0, 0.0};
    double near_src[3] = {2.0, 0.0, 0.0};
    double mid_src[3] = {6.0, 0.0, 0.0};
    double fast_src[3] = {10.0, 0.0, 0.0};
    double fast_vel[3] = {-100.0, 0.0, 0.0};
    int64_t volume = 0;
    int64_t pan = 0;
    double doppler = 1.0;

    rt_sound3d_listener_state_set(&listener, listener_pos, forward, nullptr);
    rt_sound3d_compute_voice_params_ex(
        &listener, near_src, nullptr, 2.0, 10.0, 100, &volume, &pan, &doppler);
    EXPECT_NEAR(volume, 100, 0.001, "Sound3D keeps full volume inside reference distance");
    EXPECT_NEAR(doppler, 1.0, 0.001, "Sound3D reports neutral Doppler without velocity");

    rt_sound3d_compute_voice_params_ex(
        &listener, mid_src, nullptr, 2.0, 10.0, 100, &volume, &pan, &doppler);
    EXPECT_NEAR(volume, 50, 0.001, "Sound3D attenuates between reference and max distance");
    EXPECT_TRUE(pan > 90, "Sound3D still pans right-side sources");

    rt_sound3d_compute_voice_params_ex(
        &listener, fast_src, fast_vel, 1.0, 50.0, 100, &volume, &pan, &doppler);
    EXPECT_TRUE(doppler > 1.0, "Sound3D Doppler factor rises for approaching sources");

    rt_sound3d_listener_state_set_pose(&listener, listener_pos, forward, tilted_up, nullptr);
    EXPECT_NEAR(listener.up[0], 1.0, 0.001, "Sound3D listener pose preserves explicit up X");
    EXPECT_NEAR(listener.right[1], -1.0, 0.001, "Sound3D listener pose derives right from up");

    void *source = rt_soundsource3d_new(nullptr);
    rt_sound3d_set_active_listener_state(&listener);
    rt_soundsource3d_set_position(source, rt_vec3_new(10.0, 0.0, 0.0));
    rt_soundsource3d_set_velocity(source, rt_vec3_new(-100.0, 0.0, 0.0));
    EXPECT_TRUE(rt_soundsource3d_get_doppler_factor(source) > 1.0,
                "SoundSource3D exposes computed Doppler factor");
}

static void test_listener_up_vector_round_trips_to_active_state() {
    void *listener = rt_soundlistener3d_new();
    void *up;
    rt_sound3d_listener_state state;

    rt_soundlistener3d_set_forward(listener, rt_vec3_new(0.0, 0.0, -1.0));
    rt_soundlistener3d_set_up(listener, rt_vec3_new(1.0, 0.0, 0.0));
    up = rt_soundlistener3d_get_up(listener);
    EXPECT_NEAR(rt_vec3_x(up), 1.0, 0.001, "SoundListener3D.Up X round-trips");
    EXPECT_NEAR(rt_vec3_y(up), 0.0, 0.001, "SoundListener3D.Up Y round-trips");
    EXPECT_NEAR(rt_vec3_z(up), 0.0, 0.001, "SoundListener3D.Up Z round-trips");

    rt_soundlistener3d_set_is_active(listener, 1);
    rt_sound3d_get_effective_listener_state(&state);
    EXPECT_NEAR(state.up[0], 1.0, 0.001, "active listener state carries Up X");
    EXPECT_NEAR(state.right[1], -1.0, 0.001, "active listener state derives right from Up");
}

static void test_object_getters_sanitize_corrupt_private_state() {
    auto *listener = (SoundListener3DTestLayout *)rt_soundlistener3d_new();
    auto *source = (SoundSource3DTestLayout *)rt_soundsource3d_new(nullptr);
    EXPECT_TRUE(listener != nullptr && source != nullptr,
                "Sound3D object corrupt getter test creates objects");

    listener->bound_node = nullptr;
    listener->bound_camera = nullptr;
    listener->state.position[0] = std::numeric_limits<double>::infinity();
    listener->state.position[1] = -std::numeric_limits<double>::infinity();
    listener->state.position[2] = std::numeric_limits<double>::quiet_NaN();
    listener->state.forward[0] = std::numeric_limits<double>::quiet_NaN();
    listener->state.forward[1] = 0.0;
    listener->state.forward[2] = 0.0;
    listener->state.up[0] = std::numeric_limits<double>::infinity();
    listener->state.up[1] = std::numeric_limits<double>::quiet_NaN();
    listener->state.up[2] = 0.0;
    listener->state.velocity[0] = 1.0e300;
    listener->state.velocity[1] = -1.0e300;
    listener->state.velocity[2] = std::numeric_limits<double>::quiet_NaN();
    listener->last_sync_position[0] = std::numeric_limits<double>::infinity();
    listener->last_sync_position[1] = -std::numeric_limits<double>::infinity();
    listener->last_sync_position[2] = std::numeric_limits<double>::quiet_NaN();
    listener->has_last_sync_position = -5;
    listener->is_active = -7;

    void *listener_pos = rt_soundlistener3d_get_position(listener);
    void *listener_forward = rt_soundlistener3d_get_forward(listener);
    void *listener_up = rt_soundlistener3d_get_up(listener);
    void *listener_velocity = rt_soundlistener3d_get_velocity(listener);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(listener_pos)) && std::isfinite(rt_vec3_y(listener_pos)) &&
                    std::isfinite(rt_vec3_z(listener_pos)),
                "SoundListener3D.GetPosition sanitizes corrupt coordinates");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(listener_forward)) &&
                    std::isfinite(rt_vec3_y(listener_forward)) &&
                    std::isfinite(rt_vec3_z(listener_forward)),
                "SoundListener3D.GetForward sanitizes corrupt basis");
    EXPECT_TRUE(std::isfinite(rt_vec3_x(listener_up)) && std::isfinite(rt_vec3_y(listener_up)) &&
                    std::isfinite(rt_vec3_z(listener_up)),
                "SoundListener3D.GetUp sanitizes corrupt basis");
    EXPECT_TRUE(std::fabs(rt_vec3_x(listener_velocity)) <= 1000000.0 &&
                    std::fabs(rt_vec3_y(listener_velocity)) <= 1000000.0 &&
                    std::isfinite(rt_vec3_z(listener_velocity)),
                "SoundListener3D.GetVelocity keeps corrupt velocity within Doppler bounds");
    EXPECT_TRUE(rt_soundlistener3d_get_is_active(listener) == 0,
                "SoundListener3D.IsActive follows the unique active-listener identity");
    EXPECT_TRUE(std::isfinite(listener->state.position[0]) &&
                    std::isfinite(listener->state.forward[0]) &&
                    std::isfinite(listener->state.up[0]),
                "SoundListener3D getters persist repaired pose state");
    EXPECT_TRUE(std::fabs(listener->state.velocity[0]) <= 1000000.0 &&
                    std::fabs(listener->state.velocity[1]) <= 1000000.0 &&
                    std::isfinite(listener->state.velocity[2]),
                "SoundListener3D getters persist repaired velocity state");
    EXPECT_TRUE(listener->has_last_sync_position == 1 &&
                    std::isfinite(listener->last_sync_position[0]) &&
                    std::isfinite(listener->last_sync_position[1]) &&
                    std::isfinite(listener->last_sync_position[2]),
                "SoundListener3D repairs retained synchronization state");

    source->bound_node = nullptr;
    source->position[0] = std::numeric_limits<double>::infinity();
    source->position[1] = -std::numeric_limits<double>::infinity();
    source->position[2] = std::numeric_limits<double>::quiet_NaN();
    source->velocity[0] = 1.0e300;
    source->velocity[1] = -1.0e300;
    source->velocity[2] = std::numeric_limits<double>::quiet_NaN();
    source->doppler_factor = std::numeric_limits<double>::quiet_NaN();
    source->last_sync_position[0] = std::numeric_limits<double>::infinity();
    source->last_sync_position[1] = -std::numeric_limits<double>::infinity();
    source->last_sync_position[2] = std::numeric_limits<double>::quiet_NaN();
    source->has_last_sync_position = -4;
    source->ref_distance = std::numeric_limits<double>::infinity();
    source->max_distance = -std::numeric_limits<double>::infinity();
    source->volume = 500;
    source->looping = -9;
    source->voice_id = -42;
    source->pitch = std::numeric_limits<double>::max();
    source->occlusion = std::numeric_limits<double>::quiet_NaN();
    source->mix_group = std::numeric_limits<int64_t>::max();
    void *source_pos = rt_soundsource3d_get_position(source);
    void *source_velocity = rt_soundsource3d_get_velocity(source);
    EXPECT_TRUE(std::isfinite(rt_vec3_x(source_pos)) && std::isfinite(rt_vec3_y(source_pos)) &&
                    std::isfinite(rt_vec3_z(source_pos)),
                "SoundSource3D.GetPosition sanitizes corrupt coordinates");
    EXPECT_TRUE(std::fabs(rt_vec3_x(source_velocity)) <= 1000000.0 &&
                    std::fabs(rt_vec3_y(source_velocity)) <= 1000000.0 &&
                    std::isfinite(rt_vec3_z(source_velocity)),
                "SoundSource3D.GetVelocity keeps corrupt velocity within Doppler bounds");
    EXPECT_TRUE(rt_soundsource3d_get_volume(source) == 100,
                "SoundSource3D.Volume getter clamps corrupt private volume");
    EXPECT_TRUE(rt_soundsource3d_get_looping(source) == 1,
                "SoundSource3D.Looping getter normalizes corrupt nonzero flags");
    EXPECT_TRUE(rt_soundsource3d_get_is_playing(source) == 0,
                "SoundSource3D.IsPlaying rejects corrupt negative voice ids");
    EXPECT_TRUE(rt_soundsource3d_get_voice_id(source) == 0,
                "SoundSource3D.VoiceId clears corrupt negative voice ids");
    EXPECT_TRUE(std::isfinite(source->position[0]) && std::isfinite(source->position[1]) &&
                    std::isfinite(source->position[2]),
                "SoundSource3D getters persist repaired coordinates");
    EXPECT_TRUE(std::fabs(source->velocity[0]) <= 1000000.0 &&
                    std::fabs(source->velocity[1]) <= 1000000.0 &&
                    std::isfinite(source->velocity[2]),
                "SoundSource3D getters persist repaired velocity");
    EXPECT_NEAR(source->doppler_factor, 1.0, 0.001, "SoundSource3D repairs cached Doppler");
    EXPECT_NEAR(rt_soundsource3d_get_ref_distance(source),
                1.0,
                0.001,
                "SoundSource3D repairs corrupt reference distance");
    EXPECT_NEAR(rt_soundsource3d_get_max_distance(source),
                0.0,
                0.001,
                "SoundSource3D repairs corrupt maximum distance");
    EXPECT_NEAR(rt_soundsource3d_get_pitch(source),
                4.0,
                0.001,
                "SoundSource3D caps retained pitch to mixer range");
    EXPECT_NEAR(rt_soundsource3d_get_occlusion(source),
                0.0,
                0.001,
                "SoundSource3D repairs corrupt occlusion");
    EXPECT_TRUE(rt_soundsource3d_get_mix_group(source) == RT_MIXGROUP_SFX,
                "SoundSource3D repairs corrupt mix group to SFX");
    EXPECT_TRUE(source->has_last_sync_position == 1 &&
                    std::isfinite(source->last_sync_position[0]) &&
                    std::isfinite(source->last_sync_position[1]) &&
                    std::isfinite(source->last_sync_position[2]),
                "SoundSource3D repairs retained synchronization state");

    rt_soundsource3d_set_position_vec(source, 1.0e300, -1.0e300, 12.0);
    rt_soundsource3d_rebase_origin(source,
                                   -1.0e300,
                                   std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN());
    source_pos = rt_soundsource3d_get_position(source);
    EXPECT_TRUE(std::fabs(rt_vec3_x(source_pos)) <= 1000000000000.0 &&
                    std::fabs(rt_vec3_y(source_pos)) <= 1000000000000.0 &&
                    std::fabs(rt_vec3_z(source_pos)) <= 1000000000000.0,
                "SoundSource3D origin rebase remains within coordinate bounds");
}

static void test_source_play_stop_when_audio_is_available() {
    if (!rt_audio_is_available()) {
        EXPECT_TRUE(true, "SoundSource3D playback skipped when audio backend is unavailable");
        return;
    }
    if (!rt_audio_init()) {
        EXPECT_TRUE(true, "SoundSource3D playback skipped when audio init fails");
        return;
    }

    void *listener = rt_soundlistener3d_new();
    void *sound = rt_synth_tone(440, 200, 0);
    void *source = rt_soundsource3d_new(sound);
    int64_t voice;

    rt_soundlistener3d_set_position(listener, rt_vec3_new(0.0, 0.0, 0.0));
    rt_soundlistener3d_set_forward(listener, rt_vec3_new(0.0, 0.0, -1.0));
    rt_soundlistener3d_set_is_active(listener, 1);
    rt_soundsource3d_set_position(source, rt_vec3_new(2.0, 0.0, 0.0));
    rt_soundsource3d_set_volume(source, 75);
    rt_soundsource3d_set_max_distance(source, 12.0);
    rt_soundsource3d_set_pitch(source, 1.5);
    rt_soundsource3d_set_occlusion(source, 0.75);
    voice = rt_soundsource3d_play(source);

    EXPECT_TRUE(voice > 0, "SoundSource3D.Play returns a live voice when audio is available");
    EXPECT_TRUE(rt_soundsource3d_get_is_playing(source) != 0,
                "SoundSource3D reports IsPlaying after starting playback");
    EXPECT_TRUE(rt_soundsource3d_get_voice_id(source) == voice,
                "SoundSource3D exposes the playing voice id");
    EXPECT_NEAR(rt_voice_get_pitch(voice),
                1.5,
                0.01,
                "SoundSource3D applies authored pitch to a newly started voice");

    rt_soundsource3d_stop(source);
    EXPECT_TRUE(rt_soundsource3d_get_is_playing(source) == 0,
                "SoundSource3D.Stop clears active playback state");
    EXPECT_TRUE(rt_soundsource3d_get_voice_id(source) == 0, "SoundSource3D.Stop clears VoiceId");
}

int main() {
    test_listener_follows_bound_camera();
    test_source_follows_bound_node_in_world_space();
    test_bound_source_manual_velocity_survives_one_sync();
    test_listener_bound_node_direction_ignores_large_translation();
    test_invalid_audio_handles_are_ignored();
    test_reference_distance_and_doppler_math();
    test_listener_up_vector_round_trips_to_active_state();
    test_object_getters_sanitize_corrupt_private_state();
    test_source_play_stop_when_audio_is_available();

    std::printf("Sound3D object tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
