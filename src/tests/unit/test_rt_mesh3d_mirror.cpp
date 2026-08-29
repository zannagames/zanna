//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_rt_mesh3d_mirror.cpp
// Purpose: ADR 0306 — `Mesh3D.Mirror` produces the left/right-mirrored copy
//          of a skinned mesh: positions/normals/tangents reflected across
//          X = 0, triangle winding reversed, tangent handedness negated,
//          bone influences remapped to their sagittal partners (side-token
//          swap, humanoid-role flip, else self), morph deltas reflected,
//          mirror∘mirror ≈ identity, and NULL for a non-skeleton handle.
// Key invariants:
//   - The source mesh is never mutated.
//   - Center bones map to themselves; unmatched sides stay put.
// Ownership/Lifetime:
//   - Every handle created here is GC-managed by the runtime under test.
//   - The test owns nothing beyond the process lifetime.
// Links: docs/adr/0306-mesh3d-mirror.md, src/runtime/graphics/3d/render/rt_mesh3d.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif
#include "rt.hpp"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_internal.h"
#include "rt_morphtarget3d.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
extern void *rt_mat4_identity(void);
}

static int tests_passed = 0;
static int tests_run = 0;

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s\n", msg);                                                    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps, msg)                                                                \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (fabs((double)(a) - (double)(b)) > (eps)) {                                             \
            fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", msg, (double)(a), (double)(b));    \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

static void *make_skeleton(int64_t *hips, int64_t *left, int64_t *right, int64_t *lone) {
    void *skel = rt_skeleton3d_new();
    rt_string hips_name = rt_const_cstr("Hips");
    rt_string left_name = rt_const_cstr("LeftArm");
    rt_string right_name = rt_const_cstr("RightArm");
    rt_string lone_name = rt_const_cstr("Tail");
    *hips = rt_skeleton3d_add_bone(skel, hips_name, -1, rt_mat4_identity());
    *left = rt_skeleton3d_add_bone(skel, left_name, 0, rt_mat4_identity());
    *right = rt_skeleton3d_add_bone(skel, right_name, 0, rt_mat4_identity());
    *lone = rt_skeleton3d_add_bone(skel, lone_name, 0, rt_mat4_identity());
    rt_string_unref(hips_name);
    rt_string_unref(left_name);
    rt_string_unref(right_name);
    rt_string_unref(lone_name);
    return skel;
}

// A strip of four vertices: two on the left arm (x > 0), one at the hips
// (x = 0), one on the tail; two triangles.
static void *make_strip(void *skel, int64_t hips, int64_t left, int64_t lone) {
    void *mesh = rt_mesh3d_new();
    rt_mesh3d_add_vertex(mesh, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 2.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0);
    rt_mesh3d_add_vertex(mesh, 0.0, 2.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
    rt_mesh3d_add_vertex(mesh, 0.5, 3.0, 2.0, 0.0, 1.0, 0.0, 1.0, 1.0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(mesh, 1, 3, 2);
    rt_mesh3d_set_skeleton(mesh, skel);
    rt_mesh3d_set_bone_weights(mesh, 0, left, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    rt_mesh3d_set_bone_weights(mesh, 1, left, 0.75, hips, 0.25, 0, 0.0, 0, 0.0);
    rt_mesh3d_set_bone_weights(mesh, 2, hips, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    rt_mesh3d_set_bone_weights(mesh, 3, lone, 1.0, 0, 0.0, 0, 0.0, 0, 0.0);
    return mesh;
}

static void test_mirror_reflects_geometry_and_winding() {
    int64_t hips, left, right, lone;
    void *skel = make_skeleton(&hips, &left, &right, &lone);
    void *mesh = make_strip(skel, hips, left, lone);
    rt_mesh3d *src = static_cast<rt_mesh3d *>(mesh);
    void *out = rt_mesh3d_mirror(mesh, skel);
    rt_mesh3d *dst = static_cast<rt_mesh3d *>(out);
    EXPECT_TRUE(out != NULL, "mirror returns a mesh");
    if (!out)
        return;
    EXPECT_TRUE(out != mesh, "mirror is a new object");
    EXPECT_TRUE(dst->vertex_count == 4 && dst->index_count == 6, "mirror keeps the counts");
    EXPECT_NEAR(dst->vertices[0].pos[0], -1.0, 1e-6, "x reflects (v0)");
    EXPECT_NEAR(dst->vertices[1].pos[0], -2.0, 1e-6, "x reflects (v1)");
    EXPECT_NEAR(dst->vertices[3].pos[0], -0.5, 1e-6, "x reflects (v3)");
    EXPECT_NEAR(dst->vertices[1].pos[1], 1.0, 1e-6, "y untouched");
    EXPECT_NEAR(dst->vertices[3].pos[2], 2.0, 1e-6, "z untouched");
    EXPECT_NEAR(dst->vertices[0].normal[0], -1.0, 1e-6, "normal x reflects");
    EXPECT_NEAR(dst->vertices[1].normal[1], 1.0, 1e-6, "normal y untouched");
    EXPECT_TRUE(dst->indices[0] == 0 && dst->indices[1] == 2 && dst->indices[2] == 1,
                "winding reversed (tri 0)");
    EXPECT_TRUE(dst->indices[3] == 1 && dst->indices[4] == 2 && dst->indices[5] == 3,
                "winding reversed (tri 1)");
    EXPECT_TRUE(dst->vertices[0].tangent[3] * src->vertices[0].tangent[3] < 0.0f,
                "tangent handedness negated");
    // The source is untouched.
    EXPECT_NEAR(src->vertices[0].pos[0], 1.0, 1e-6, "source x untouched");
    EXPECT_TRUE(src->indices[1] == 1 && src->indices[2] == 2, "source winding untouched");
}

static void test_mirror_remaps_bone_influences() {
    int64_t hips, left, right, lone;
    void *skel = make_skeleton(&hips, &left, &right, &lone);
    void *mesh = make_strip(skel, hips, left, lone);
    rt_mesh3d *dst = static_cast<rt_mesh3d *>(rt_mesh3d_mirror(mesh, skel));
    EXPECT_TRUE(dst != NULL, "mirror returns a mesh");
    if (!dst)
        return;
    EXPECT_TRUE(dst->vertices[0].bone_indices[0] == (uint8_t)right, "LeftArm -> RightArm (v0)");
    EXPECT_TRUE(dst->vertices[1].bone_indices[0] == (uint8_t)right, "LeftArm -> RightArm (v1)");
    EXPECT_TRUE(dst->vertices[1].bone_indices[1] == (uint8_t)hips, "Hips stays (v1)");
    EXPECT_NEAR(dst->vertices[1].bone_weights[0], 0.75, 1e-6, "weights untouched");
    EXPECT_TRUE(dst->vertices[2].bone_indices[0] == (uint8_t)hips, "center bone self-maps");
    EXPECT_TRUE(dst->vertices[3].bone_indices[0] == (uint8_t)lone, "unpaired bone self-maps");
    EXPECT_TRUE(rt_skeleton3d_mirror_bone(skel, (int32_t)left) == (int32_t)right,
                "helper: LeftArm -> RightArm");
    EXPECT_TRUE(rt_skeleton3d_mirror_bone(skel, (int32_t)right) == (int32_t)left,
                "helper: RightArm -> LeftArm");
    EXPECT_TRUE(rt_skeleton3d_mirror_bone(skel, (int32_t)hips) == (int32_t)hips,
                "helper: Hips -> Hips");
    EXPECT_TRUE(rt_skeleton3d_mirror_bone(skel, 99) == 99, "helper: out of range unchanged");
    // The attached skeleton serves when no skeleton is passed.
    rt_mesh3d *dst2 = static_cast<rt_mesh3d *>(rt_mesh3d_mirror(mesh, NULL));
    EXPECT_TRUE(dst2 != NULL && dst2->vertices[0].bone_indices[0] == (uint8_t)right,
                "attached skeleton resolves partners");
}

static void test_mirror_twice_is_identity() {
    int64_t hips, left, right, lone;
    void *skel = make_skeleton(&hips, &left, &right, &lone);
    void *mesh = make_strip(skel, hips, left, lone);
    rt_mesh3d *src = static_cast<rt_mesh3d *>(mesh);
    void *once = rt_mesh3d_mirror(mesh, skel);
    rt_mesh3d *twice = static_cast<rt_mesh3d *>(rt_mesh3d_mirror(once, skel));
    EXPECT_TRUE(twice != NULL, "double mirror returns a mesh");
    if (!twice)
        return;
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(twice->vertices[i].pos[0], src->vertices[i].pos[0], 1e-6, "x restored");
        EXPECT_NEAR(
            twice->vertices[i].normal[0], src->vertices[i].normal[0], 1e-6, "normal restored");
        EXPECT_TRUE(twice->vertices[i].bone_indices[0] == src->vertices[i].bone_indices[0],
                    "bone restored");
    }
    for (uint32_t i = 0; i < 6; ++i)
        EXPECT_TRUE(twice->indices[i] == src->indices[i], "winding restored");
}

static void test_mirror_reflects_morph_deltas() {
    int64_t hips, left, right, lone;
    void *skel = make_skeleton(&hips, &left, &right, &lone);
    void *mesh = make_strip(skel, hips, left, lone);
    void *morph = rt_morphtarget3d_new(4);
    rt_string shape_name = rt_const_cstr("smile");
    int64_t shape = rt_morphtarget3d_add_shape(morph, shape_name);
    rt_string_unref(shape_name);
    rt_morphtarget3d_set_delta(morph, shape, 1, 0.5, 0.25, -0.75);
    rt_mesh3d_set_morph_targets(mesh, morph);
    rt_mesh3d *dst = static_cast<rt_mesh3d *>(rt_mesh3d_mirror(mesh, skel));
    EXPECT_TRUE(dst != NULL && dst->morph_targets_ref != NULL, "mirror carries morph targets");
    if (!dst || !dst->morph_targets_ref)
        return;
    EXPECT_TRUE(dst->morph_targets_ref != morph, "morph targets are a new object");
    void *mirrored = rt_morphtarget3d_clone_mirrored_x(morph);
    EXPECT_TRUE(mirrored != NULL, "mirrored morph clone");
    EXPECT_NEAR(rt_morphtarget3d_delta_lane_internal(mirrored, shape, 1, 0),
                -0.5,
                1e-6,
                "delta x reflected");
    EXPECT_NEAR(rt_morphtarget3d_delta_lane_internal(mirrored, shape, 1, 1),
                0.25,
                1e-6,
                "delta y untouched");
    EXPECT_NEAR(rt_morphtarget3d_delta_lane_internal(mirrored, shape, 1, 2),
                -0.75,
                1e-6,
                "delta z untouched");
    EXPECT_NEAR(rt_morphtarget3d_delta_lane_internal(dst->morph_targets_ref, shape, 1, 0),
                -0.5,
                1e-6,
                "mesh carries the reflected deltas");
    EXPECT_NEAR(rt_morphtarget3d_delta_lane_internal(morph, shape, 1, 0),
                0.5,
                1e-6,
                "source deltas untouched");
}

static void test_mirror_rejects_bad_inputs() {
    int64_t hips, left, right, lone;
    void *skel = make_skeleton(&hips, &left, &right, &lone);
    void *mesh = make_strip(skel, hips, left, lone);
    EXPECT_TRUE(rt_mesh3d_mirror(NULL, skel) == NULL, "NULL mesh -> NULL");
    EXPECT_TRUE(rt_mesh3d_mirror(mesh, mesh) == NULL, "non-skeleton handle -> NULL");
    // A weightless mesh mirrors as plain geometry.
    void *plain = rt_mesh3d_new_box(2.0, 1.0, 1.0);
    rt_mesh3d *pm = static_cast<rt_mesh3d *>(rt_mesh3d_mirror(plain, NULL));
    EXPECT_TRUE(pm != NULL && pm->vertex_count == static_cast<rt_mesh3d *>(plain)->vertex_count,
                "weightless mesh mirrors");
}

int main() {
    test_mirror_reflects_geometry_and_winding();
    test_mirror_remaps_bone_influences();
    test_mirror_twice_is_identity();
    test_mirror_reflects_morph_deltas();
    test_mirror_rejects_bad_inputs();
    printf("%d/%d mesh3d mirror tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
