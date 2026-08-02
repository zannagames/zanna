//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_transform_path.cpp
// Purpose: Unit tests for Transform3D and Path3D.
//
// Links: rt_transform3d.h, rt_path3d.h
//
//===----------------------------------------------------------------------===//

#include "rt.hpp"
#include "rt_graphics3d_ids.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_path3d.h"
#include "rt_transform3d.h"
#include <cassert>
#include <cmath>
#include <cstdio>

extern "C" {
extern void *rt_vec3_new(double x, double y, double z);
extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);
extern void *rt_quat_new(double x, double y, double z, double w);
extern double rt_quat_x(void *q);
extern double rt_quat_y(void *q);
extern double rt_quat_z(void *q);
extern double rt_quat_w(void *q);
extern void *rt_mat4_new(double m0,
                         double m1,
                         double m2,
                         double m3,
                         double m4,
                         double m5,
                         double m6,
                         double m7,
                         double m8,
                         double m9,
                         double m10,
                         double m11,
                         double m12,
                         double m13,
                         double m14,
                         double m15);
extern void *rt_mat4_ortho(
    double left, double right, double bottom, double top, double near, double far);
extern double rt_mat4_get(void *m, int64_t r, int64_t c);
extern void *rt_mat4_transform_point(void *m, void *v);
extern void *rt_mat4_inverse(void *m);
}

static int tests_passed = 0;
static int tests_run = 0;
static constexpr double kPi = 3.14159265358979323846;

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

/*==========================================================================
 * Transform3D tests
 *=========================================================================*/

static void test_transform_default() {
    void *xf = rt_transform3d_new();
    EXPECT_TRUE(xf != nullptr, "Transform3D created");

    void *pos = rt_transform3d_get_position(xf);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.01, "Default pos X = 0");
    EXPECT_NEAR(rt_vec3_y(pos), 0.0, 0.01, "Default pos Y = 0");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.01, "Default pos Z = 0");

    void *scl = rt_transform3d_get_scale(xf);
    EXPECT_NEAR(rt_vec3_x(scl), 1.0, 0.01, "Default scale X = 1");
    EXPECT_NEAR(rt_vec3_y(scl), 1.0, 0.01, "Default scale Y = 1");
    EXPECT_NEAR(rt_vec3_z(scl), 1.0, 0.01, "Default scale Z = 1");
}

static void test_transform_position() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, 3.0, 5.0, 7.0);

    void *mat = rt_transform3d_get_matrix(xf);
    /* Translation is in column 3 (row-major: indices 3, 7, 11) */
    EXPECT_NEAR(rt_mat4_get(mat, 0, 3), 3.0, 0.01, "Matrix tx = 3");
    EXPECT_NEAR(rt_mat4_get(mat, 1, 3), 5.0, 0.01, "Matrix ty = 5");
    EXPECT_NEAR(rt_mat4_get(mat, 2, 3), 7.0, 0.01, "Matrix tz = 7");
}

static void test_transform_scale() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_scale(xf, 2.0, 3.0, 4.0);

    void *mat = rt_transform3d_get_matrix(xf);
    /* For identity rotation, diagonal = scale */
    EXPECT_NEAR(rt_mat4_get(mat, 0, 0), 2.0, 0.01, "Matrix sx = 2");
    EXPECT_NEAR(rt_mat4_get(mat, 1, 1), 3.0, 0.01, "Matrix sy = 3");
    EXPECT_NEAR(rt_mat4_get(mat, 2, 2), 4.0, 0.01, "Matrix sz = 4");
}

static void test_transform_translate() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, 1.0, 2.0, 3.0);
    rt_transform3d_translate(xf, rt_vec3_new(10.0, 20.0, 30.0));

    void *pos = rt_transform3d_get_position(xf);
    EXPECT_NEAR(rt_vec3_x(pos), 11.0, 0.01, "Translate: X = 1+10 = 11");
    EXPECT_NEAR(rt_vec3_y(pos), 22.0, 0.01, "Translate: Y = 2+20 = 22");
    EXPECT_NEAR(rt_vec3_z(pos), 33.0, 0.01, "Translate: Z = 3+30 = 33");
}

static void test_transform_euler() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_euler(xf, 0.0, 90.0, 0.0);

    void *rot = rt_transform3d_get_rotation(xf);
    EXPECT_NEAR(fabs(rt_quat_y(rot)), 0.707, 0.02, "Euler yaw 90° maps to quaternion Y");
    EXPECT_NEAR(fabs(rt_quat_w(rot)), 0.707, 0.02, "Euler yaw 90° maps to quaternion W");
    EXPECT_NEAR(fabs(rt_quat_x(rot)), 0.0, 0.02, "Euler yaw 90° does not leak into X");
    EXPECT_NEAR(fabs(rt_quat_z(rot)), 0.0, 0.02, "Euler yaw 90° does not leak into Z");

    rt_transform3d_set_euler(xf, 90.0, 0.0, 0.0);
    rot = rt_transform3d_get_rotation(xf);
    EXPECT_NEAR(fabs(rt_quat_x(rot)), 0.707, 0.02, "Euler pitch 90° maps to quaternion X");
    EXPECT_NEAR(fabs(rt_quat_y(rot)), 0.0, 0.02, "Euler pitch 90° does not leak into Y");
    EXPECT_NEAR(fabs(rt_quat_z(rot)), 0.0, 0.02, "Euler pitch 90° does not leak into Z");

    rt_transform3d_set_euler(xf, 0.0, 0.0, 90.0);
    rot = rt_transform3d_get_rotation(xf);
    EXPECT_NEAR(fabs(rt_quat_z(rot)), 0.707, 0.02, "Euler roll 90° maps to quaternion Z");
    EXPECT_NEAR(fabs(rt_quat_x(rot)), 0.0, 0.02, "Euler roll 90° does not leak into X");
    EXPECT_NEAR(fabs(rt_quat_y(rot)), 0.0, 0.02, "Euler roll 90° does not leak into Y");
}

static void test_transform_look_at_uses_negative_z_forward() {
    void *xf = rt_transform3d_new();
    rt_transform3d_look_at(xf, rt_vec3_new(0.0, 0.0, -1.0), rt_vec3_new(0.0, 1.0, 0.0));
    void *rot = rt_transform3d_get_rotation(xf);
    EXPECT_NEAR(rt_quat_x(rot), 0.0, 0.02, "LookAt -Z target keeps identity X");
    EXPECT_NEAR(rt_quat_y(rot), 0.0, 0.02, "LookAt -Z target keeps identity Y");
    EXPECT_NEAR(rt_quat_z(rot), 0.0, 0.02, "LookAt -Z target keeps identity Z");
    EXPECT_NEAR(fabs(rt_quat_w(rot)), 1.0, 0.02, "LookAt -Z target keeps identity W");
}

static void test_transform_rejects_wrong_value_handles() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, 1.0, 2.0, 3.0);
    rt_transform3d_translate(xf, xf);
    void *pos = rt_transform3d_get_position(xf);
    EXPECT_NEAR(rt_vec3_x(pos), 1.0, 0.01, "Translate rejects non-Vec3 X");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.01, "Translate rejects non-Vec3 Y");
    EXPECT_NEAR(rt_vec3_z(pos), 3.0, 0.01, "Translate rejects non-Vec3 Z");

    rt_transform3d_set_rotation(xf, xf);
    void *rot = rt_transform3d_get_rotation(xf);
    EXPECT_NEAR(rt_quat_x(rot), 0.0, 0.01, "SetRotation rejects non-Quat X");
    EXPECT_NEAR(rt_quat_y(rot), 0.0, 0.01, "SetRotation rejects non-Quat Y");
    EXPECT_NEAR(rt_quat_z(rot), 0.0, 0.01, "SetRotation rejects non-Quat Z");
    EXPECT_NEAR(rt_quat_w(rot), 1.0, 0.01, "SetRotation rejects non-Quat W");
}

static void test_mat4_ortho_translation_layout() {
    void *ortho = rt_mat4_ortho(0.0, 10.0, 0.0, 20.0, 1.0, 101.0);
    void *center = rt_mat4_transform_point(ortho, rt_vec3_new(5.0, 10.0, -51.0));
    EXPECT_NEAR(rt_vec3_x(center), 0.0, 0.01, "Mat4.Ortho maps center X to NDC 0");
    EXPECT_NEAR(rt_vec3_y(center), 0.0, 0.01, "Mat4.Ortho maps center Y to NDC 0");
    EXPECT_NEAR(rt_vec3_z(center), 0.0, 0.01, "Mat4.Ortho maps center Z to NDC 0");
}

static void test_mat4_inverse_translation_layout() {
    void *translate = rt_mat4_new(
        1.0, 0.0, 0.0, 2.0, 0.0, 1.0, 0.0, -3.0, 0.0, 0.0, 1.0, 5.0, 0.0, 0.0, 0.0, 1.0);
    void *inverse = rt_mat4_inverse(translate);
    void *local = rt_mat4_transform_point(inverse, rt_vec3_new(7.0, 8.0, 9.0));
    EXPECT_NEAR(rt_vec3_x(local), 5.0, 0.01, "Mat4.Inverse subtracts row-major translation X");
    EXPECT_NEAR(rt_vec3_y(local), 11.0, 0.01, "Mat4.Inverse subtracts row-major translation Y");
    EXPECT_NEAR(rt_vec3_z(local), 4.0, 0.01, "Mat4.Inverse subtracts row-major translation Z");
}

static void test_transform_dirty_flag() {
    void *xf = rt_transform3d_new();
    void *mat1 = rt_transform3d_get_matrix(xf);     /* triggers compute */
    rt_transform3d_set_position(xf, 5.0, 0.0, 0.0); /* marks dirty */
    void *mat2 = rt_transform3d_get_matrix(xf);     /* triggers recompute */
    EXPECT_NEAR(rt_mat4_get(mat2, 0, 3), 5.0, 0.01, "Dirty flag: new position reflected");
    (void)mat1;
}

static void test_transform_sanitizes_nonfinite_inputs() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, NAN, 2.0, INFINITY);
    void *pos = rt_transform3d_get_position(xf);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.01, "Transform non-finite pos X falls back to 0");
    EXPECT_NEAR(rt_vec3_y(pos), 2.0, 0.01, "Transform finite pos Y is preserved");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.01, "Transform non-finite pos Z falls back to 0");

    rt_transform3d_set_scale(xf, NAN, -2.0, INFINITY);
    void *scale = rt_transform3d_get_scale(xf);
    EXPECT_NEAR(rt_vec3_x(scale), 1.0, 0.01, "Transform non-finite scale X falls back to 1");
    EXPECT_NEAR(rt_vec3_y(scale), -2.0, 0.01, "Transform finite negative scale is preserved");
    EXPECT_NEAR(rt_vec3_z(scale), 1.0, 0.01, "Transform non-finite scale Z falls back to 1");

    rt_transform3d_set_rotation(xf, rt_quat_new(NAN, 0.0, 0.0, 0.0));
    void *rot = rt_transform3d_get_rotation(xf);
    EXPECT_NEAR(rt_quat_x(rot), 0.0, 0.01, "Transform invalid quaternion resets X");
    EXPECT_NEAR(rt_quat_y(rot), 0.0, 0.01, "Transform invalid quaternion resets Y");
    EXPECT_NEAR(rt_quat_z(rot), 0.0, 0.01, "Transform invalid quaternion resets Z");
    EXPECT_NEAR(rt_quat_w(rot), 1.0, 0.01, "Transform invalid quaternion resets W");

    rt_transform3d_translate(xf, rt_vec3_new(INFINITY, 3.0, NAN));
    pos = rt_transform3d_get_position(xf);
    EXPECT_NEAR(rt_vec3_x(pos), 0.0, 0.01, "Transform translate ignores non-finite X delta");
    EXPECT_NEAR(rt_vec3_y(pos), 5.0, 0.01, "Transform translate applies finite Y delta");
    EXPECT_NEAR(rt_vec3_z(pos), 0.0, 0.01, "Transform translate ignores non-finite Z delta");

    rt_transform3d_set_euler(xf, NAN, INFINITY, 0.0);
    void *mat = rt_transform3d_get_matrix(xf);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            EXPECT_TRUE(std::isfinite(rt_mat4_get(mat, r, c)),
                        "Transform matrix stays finite after invalid Euler input");
}

static void test_transform_clamps_extreme_finite_inputs() {
    void *xf = rt_transform3d_new();
    rt_transform3d_set_position(xf, 1.0e300, -1.0e300, 42.0);
    void *pos = rt_transform3d_get_position(xf);
    EXPECT_NEAR(rt_vec3_x(pos), 1000000000000.0, 1.0, "Extreme position X clamps");
    EXPECT_NEAR(rt_vec3_y(pos), -1000000000000.0, 1.0, "Extreme position Y clamps");
    EXPECT_NEAR(rt_vec3_z(pos), 42.0, 0.01, "Finite position Z is preserved");

    rt_transform3d_set_scale(xf, 1.0e300, -1.0e300, 0.0);
    void *scale = rt_transform3d_get_scale(xf);
    EXPECT_NEAR(rt_vec3_x(scale), 1000000000000.0, 1.0, "Extreme positive scale clamps");
    EXPECT_NEAR(rt_vec3_y(scale), -1000000000000.0, 1.0, "Extreme negative scale clamps");
    EXPECT_NEAR(rt_vec3_z(scale), 0.0, 0.01, "Finite zero scale is preserved");

    rt_transform3d_set_rotation(xf, rt_quat_new(1.0e300, 0.0, 0.0, 0.0));
    void *rot = rt_transform3d_get_rotation(xf);
    EXPECT_TRUE(std::isfinite(rt_quat_x(rot)) && std::isfinite(rt_quat_y(rot)) &&
                    std::isfinite(rt_quat_z(rot)) && std::isfinite(rt_quat_w(rot)),
                "Extreme finite quaternion normalizes to finite components");
    EXPECT_NEAR(fabs(rt_quat_x(rot)), 1.0, 0.01, "Extreme quaternion keeps dominant axis");

    rt_transform3d_rotate(xf, rt_vec3_new(1.0e300, 0.0, 0.0), kPi);
    rot = rt_transform3d_get_rotation(xf);
    EXPECT_TRUE(std::isfinite(rt_quat_x(rot)) && std::isfinite(rt_quat_w(rot)),
                "Extreme finite rotation axis normalizes without overflow");

    rt_transform3d_look_at(xf, rt_vec3_new(1.0e300, 0.0, -1.0e300), rt_vec3_new(0.0, 1.0e300, 0.0));
    void *mat = rt_transform3d_get_matrix(xf);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            EXPECT_TRUE(std::isfinite(rt_mat4_get(mat, r, c)),
                        "Transform matrix stays finite after extreme LookAt");
}

/*==========================================================================
 * Path3D tests
 *=========================================================================*/

static void test_path_linear() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(10, 0, 0));

    void *p0 = rt_path3d_get_position_at(path, 0.0);
    EXPECT_NEAR(rt_vec3_x(p0), 0.0, 0.1, "Path t=0: X = 0");

    void *p1 = rt_path3d_get_position_at(path, 1.0);
    EXPECT_NEAR(rt_vec3_x(p1), 10.0, 0.1, "Path t=1: X = 10");

    void *pm = rt_path3d_get_position_at(path, 0.5);
    EXPECT_NEAR(rt_vec3_x(pm), 5.0, 0.5, "Path t=0.5: X ≈ 5");
}

static void test_path_catmull_rom() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(5, 3, 0));
    rt_path3d_add_point(path, rt_vec3_new(10, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(15, 3, 0));

    /* Catmull-Rom passes through control points */
    void *p0 = rt_path3d_get_position_at(path, 0.0);
    EXPECT_NEAR(rt_vec3_x(p0), 0.0, 0.1, "CatmullRom t=0: first point");

    void *p1 = rt_path3d_get_position_at(path, 1.0);
    EXPECT_NEAR(rt_vec3_x(p1), 15.0, 0.1, "CatmullRom t=1: last point");

    EXPECT_TRUE(rt_path3d_get_point_count(path) == 4, "Point count = 4");
}

static void test_path_direction() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(10, 0, 0));

    void *dir = rt_path3d_get_direction_at(path, 0.5);
    /* Direction should be roughly along +X */
    EXPECT_TRUE(rt_vec3_x(dir) > 0.9, "Direction along X for horizontal path");
    EXPECT_NEAR(rt_vec3_y(dir), 0.0, 0.1, "Direction Y ≈ 0");
}

static void test_path_length() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(10, 0, 0));

    double len = rt_path3d_get_length(path);
    EXPECT_NEAR(len, 10.0, 0.5, "Straight path length ≈ 10");
}

static void test_path_looping() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(5, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(5, 5, 0));
    rt_path3d_add_point(path, rt_vec3_new(0, 5, 0));
    rt_path3d_set_looping(path, 1);

    /* At t=0 and t=1, should be at same position (loop wraps) */
    void *p0 = rt_path3d_get_position_at(path, 0.0);
    void *p1 = rt_path3d_get_position_at(path, 1.0);
    double dx = rt_vec3_x(p1) - rt_vec3_x(p0);
    double dy = rt_vec3_y(p1) - rt_vec3_y(p0);
    EXPECT_NEAR(sqrt(dx * dx + dy * dy), 0.0, 1.0, "Looping path: t=0 ≈ t=1");
}

static void test_path_clear() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_vec3_new(0, 0, 0));
    rt_path3d_add_point(path, rt_vec3_new(5, 0, 0));
    EXPECT_TRUE(rt_path3d_get_point_count(path) == 2, "Before clear: 2 points");
    rt_path3d_clear(path);
    EXPECT_TRUE(rt_path3d_get_point_count(path) == 0, "After clear: 0 points");
}

static void test_path_rejects_non_vec3_points() {
    void *path = rt_path3d_new();
    rt_path3d_add_point(path, rt_transform3d_new());
    EXPECT_TRUE(rt_path3d_get_point_count(path) == 0, "Path3D.AddPoint rejects non-Vec3 handles");
}

static void test_path_rejects_undersized_class_spoofs() {
    void *path = rt_obj_new_i64(RT_G3D_PATH3D_CLASS_ID, 1);
    void *point = rt_vec3_new(1.0, 2.0, 3.0);
    rt_path3d_add_point(path, point);
    EXPECT_TRUE(rt_path3d_get_point_count(path) == 0,
                "Path3D rejects an undersized class-id spoof before storage access");
    EXPECT_NEAR(rt_path3d_get_length(path), 0.0, 0.0, "Undersized Path3D has zero length");
    void *position = rt_path3d_get_position_at(path, 0.5);
    EXPECT_NEAR(rt_vec3_x(position), 0.0, 0.0, "Undersized Path3D position falls back to zero");
    rt_path3d_set_looping(path, 1);
    EXPECT_TRUE(rt_path3d_get_looping(path) == 0,
                "Undersized Path3D looping mutation is ignored safely");
    rt_path3d_clear(path);
    if (position && rt_obj_release_check0(position))
        rt_obj_free(position);
    if (point && rt_obj_release_check0(point))
        rt_obj_free(point);
    if (path && rt_obj_release_check0(path))
        rt_obj_free(path);
}

int main() {
    /* Transform3D */
    test_transform_default();
    test_transform_position();
    test_transform_scale();
    test_transform_translate();
    test_transform_euler();
    test_transform_look_at_uses_negative_z_forward();
    test_transform_rejects_wrong_value_handles();
    test_mat4_ortho_translation_layout();
    test_mat4_inverse_translation_layout();
    test_transform_dirty_flag();
    test_transform_sanitizes_nonfinite_inputs();
    test_transform_clamps_extreme_finite_inputs();

    /* Path3D */
    test_path_linear();
    test_path_catmull_rom();
    test_path_direction();
    test_path_length();
    test_path_looping();
    test_path_clear();
    test_path_rejects_non_vec3_points();
    test_path_rejects_undersized_class_spoofs();

    printf("Transform3D+Path3D tests: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
