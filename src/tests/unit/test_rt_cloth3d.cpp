//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_cloth3d.cpp
// Purpose: Unit tests for the Cloth3D verlet simulator — hanging-chain
//   settling, bit-identical determinism, substep-slicing invariance, sphere
//   pushout, and pin enforcement.
// Key invariants:
//   - Tests drive rt_cloth3d_step directly (no world) with fixed dt series.
// Ownership/Lifetime:
//   - GC handles are leaked deliberately; the process exits after the run.
// Links: src/runtime/graphics/3d/physics/rt_cloth3d.c, ADR 0096
//
//===----------------------------------------------------------------------===//

#include "rt_cloth3d.h"
#include "rt_vec3.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        ++tests_run;                                                                               \
        if (cond) {                                                                                \
            ++tests_passed;                                                                        \
        } else {                                                                                   \
            std::printf("FAIL(%s:%d): %s\n", __FILE__, __LINE__, msg);                             \
        }                                                                                          \
    } while (0)

static void point_of(void *cloth, int64_t index, double out[3]) {
    void *vec = rt_cloth3d_get_point(cloth, index);
    out[0] = rt_vec3_x(vec);
    out[1] = rt_vec3_y(vec);
    out[2] = rt_vec3_z(vec);
}

/// A pinned 10-segment chain settles into a straight vertical hang whose
/// total length stays within 1% of the rest length.
static void test_chain_settles() {
    void *cloth = rt_cloth3d_new_chain(10, 1.0);
    EXPECT_TRUE(cloth != nullptr, "chain allocates");
    rt_cloth3d_pin(cloth, 0);
    for (int i = 0; i < 600; ++i)
        rt_cloth3d_step(cloth, 1.0 / 60.0);
    double total = 0.0;
    double prev[3], cur[3];
    point_of(cloth, 0, prev);
    EXPECT_TRUE(std::fabs(prev[0]) < 1e-9 && std::fabs(prev[1]) < 1e-9,
                "pinned root stays at the origin");
    for (int64_t i = 1; i <= 10; ++i) {
        point_of(cloth, i, cur);
        total += std::sqrt((cur[0] - prev[0]) * (cur[0] - prev[0]) +
                           (cur[1] - prev[1]) * (cur[1] - prev[1]) +
                           (cur[2] - prev[2]) * (cur[2] - prev[2]));
        prev[0] = cur[0];
        prev[1] = cur[1];
        prev[2] = cur[2];
    }
    EXPECT_TRUE(std::fabs(total - 1.0) < 0.01, "settled chain length within 1% of rest");
    point_of(cloth, 10, cur);
    EXPECT_TRUE(cur[1] < -0.98, "tip hangs straight down");
    EXPECT_TRUE(std::fabs(cur[0]) < 1e-6 && std::fabs(cur[2]) < 1e-6,
                "no lateral drift without wind");
}

/// Identical cloths fed identical dt series stay bit-identical.
static void test_determinism_replay() {
    void *a = rt_cloth3d_new_chain(12, 1.5);
    void *b = rt_cloth3d_new_chain(12, 1.5);
    rt_cloth3d_pin(a, 0);
    rt_cloth3d_pin(b, 0);
    void *wind = rt_vec3_new(1.0, 0.0, 0.3);
    rt_cloth3d_set_wind(a, wind, 2.0);
    rt_cloth3d_set_wind(b, wind, 2.0);
    for (int i = 0; i < 500; ++i) {
        rt_cloth3d_step(a, 1.0 / 60.0);
        rt_cloth3d_step(b, 1.0 / 60.0);
    }
    bool identical = true;
    for (int64_t p = 0; p <= 12; ++p) {
        double pa[3], pb[3];
        point_of(a, p, pa);
        point_of(b, p, pb);
        for (int k = 0; k < 3; ++k)
            if (pa[k] != pb[k])
                identical = false;
    }
    EXPECT_TRUE(identical, "500-step replay is bit-identical");
}

/// 1/60 steps and 1/240 steps cover the same simulated time with the same
/// fixed-substep sequence, so end states match exactly.
static void test_substep_slicing_invariance() {
    void *a = rt_cloth3d_new_chain(8, 1.0);
    void *b = rt_cloth3d_new_chain(8, 1.0);
    rt_cloth3d_pin(a, 0);
    rt_cloth3d_pin(b, 0);
    for (int i = 0; i < 240; ++i)
        rt_cloth3d_step(a, 1.0 / 60.0);
    for (int i = 0; i < 960; ++i)
        rt_cloth3d_step(b, 1.0 / 240.0);
    bool identical = true;
    for (int64_t p = 0; p <= 8; ++p) {
        double pa[3], pb[3];
        point_of(a, p, pa);
        point_of(b, p, pb);
        for (int k = 0; k < 3; ++k)
            if (pa[k] != pb[k])
                identical = false;
    }
    EXPECT_TRUE(identical, "frame-dt slicing does not change the substep states");
}

/// A chain draped over a sphere rests outside the sphere radius.
static void test_sphere_pushout() {
    void *cloth = rt_cloth3d_new_chain(12, 2.0);
    rt_cloth3d_pin(cloth, 0);
    void *center = rt_vec3_new(0.0, -0.6, 0.0);
    rt_cloth3d_add_sphere(cloth, center, 0.4);
    for (int i = 0; i < 600; ++i)
        rt_cloth3d_step(cloth, 1.0 / 60.0);
    bool outside = true;
    for (int64_t p = 1; p <= 12; ++p) {
        double pos[3];
        point_of(cloth, p, pos);
        double dx = pos[0], dy = pos[1] + 0.6, dz = pos[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < 0.4 - 1e-6)
            outside = false;
    }
    EXPECT_TRUE(outside, "no point penetrates the sphere collider");
}

/// Pinned points never move, even under wind.
static void test_pins_hold() {
    void *cloth = rt_cloth3d_new_patch(6, 4, 1.2, 0.6);
    for (int64_t i = 0; i < 6; ++i)
        rt_cloth3d_pin(cloth, i);
    void *wind = rt_vec3_new(0.0, 0.0, 1.0);
    rt_cloth3d_set_wind(cloth, wind, 8.0);
    double before[3];
    point_of(cloth, 3, before);
    for (int i = 0; i < 300; ++i)
        rt_cloth3d_step(cloth, 1.0 / 60.0);
    double after[3];
    point_of(cloth, 3, after);
    EXPECT_TRUE(before[0] == after[0] && before[1] == after[1] && before[2] == after[2],
                "pinned top-row point is immovable");
    double bottom[3];
    point_of(cloth, 3 * 6 + 3, bottom);
    EXPECT_TRUE(bottom[2] > 0.1, "unpinned bottom billows along the wind");
}

/// Extreme but finite public inputs stay bounded, and rejected deltas are exact no-ops.
static void test_extreme_inputs_remain_finite() {
    void *cloth = rt_cloth3d_new_chain(16, 2.0);
    rt_cloth3d_pin(cloth, 0);
    rt_cloth3d_set_gravity_scale(cloth, DBL_MAX);
    rt_cloth3d_set_wind_response(cloth, DBL_MAX);
    rt_cloth3d_set_wind(cloth, rt_vec3_new(1.0, -1.0, 0.5), DBL_MAX);
    rt_cloth3d_add_capsule(cloth, rt_vec3_new(0.0, -1.0, 0.0), rt_vec3_new(0.0, -1.0, 0.0), 0.25);

    rt_cloth3d_step(cloth, DBL_MAX);
    bool finite = true;
    for (int64_t point = 0; point < rt_cloth3d_get_point_count(cloth); ++point) {
        double position[3];
        point_of(cloth, point, position);
        finite = finite && std::isfinite(position[0]) && std::isfinite(position[1]) &&
                 std::isfinite(position[2]);
    }
    EXPECT_TRUE(finite, "extreme finite cloth inputs keep every point finite");
    EXPECT_TRUE(rt_cloth3d_get_gravity_scale(cloth) == 1000000.0 &&
                    rt_cloth3d_get_wind_response(cloth) == 120.0,
                "extreme cloth coefficients clamp to documented stability bounds");

    double before[3], after[3];
    point_of(cloth, 16, before);
    rt_cloth3d_step(cloth, NAN);
    point_of(cloth, 16, after);
    EXPECT_TRUE(before[0] == after[0] && before[1] == after[1] && before[2] == after[2],
                "non-finite cloth delta is an exact no-op");
}

int main() {
    test_chain_settles();
    test_determinism_replay();
    test_substep_slicing_invariance();
    test_sphere_pushout();
    test_pins_hold();
    test_extreme_inputs_remain_finite();
    std::printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
