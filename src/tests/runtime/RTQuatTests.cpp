//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTQuatTests.cpp
// Purpose: Tests for Zanna.Quat quaternion math utilities.
//
//===----------------------------------------------------------------------===//

#include "rt_internal.h"
#include "rt_mat4.h"
#include "rt_quat.h"
#include "rt_vec3.h"

#include <cassert>
#include <cmath>
#include <cstdio>

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static const double EPSILON = 1e-6;
static const double PI = 3.14159265358979323846;

static bool approx_eq(double a, double b) {
    return fabs(a - b) < EPSILON;
}

// ============================================================================
// Constructors
// ============================================================================

static void test_new() {
    void *q = rt_quat_new(1.0, 2.0, 3.0, 4.0);
    assert(q != nullptr);
    assert(approx_eq(rt_quat_x(q), 1.0));
    assert(approx_eq(rt_quat_y(q), 2.0));
    assert(approx_eq(rt_quat_z(q), 3.0));
    assert(approx_eq(rt_quat_w(q), 4.0));
    printf("test_new: PASSED\n");
}

static void test_identity() {
    void *q = rt_quat_identity();
    assert(q != nullptr);
    assert(approx_eq(rt_quat_x(q), 0.0));
    assert(approx_eq(rt_quat_y(q), 0.0));
    assert(approx_eq(rt_quat_z(q), 0.0));
    assert(approx_eq(rt_quat_w(q), 1.0));
    printf("test_identity: PASSED\n");
}

static void test_from_axis_angle() {
    /* 90 degrees around Z axis */
    void *axis = rt_vec3_new(0.0, 0.0, 1.0);
    void *q = rt_quat_from_axis_angle(axis, PI / 2.0);
    assert(q != nullptr);
    /* Expected: (0, 0, sin(45), cos(45)) = (0, 0, 0.7071, 0.7071) */
    assert(approx_eq(rt_quat_x(q), 0.0));
    assert(approx_eq(rt_quat_y(q), 0.0));
    assert(approx_eq(rt_quat_z(q), sin(PI / 4.0)));
    assert(approx_eq(rt_quat_w(q), cos(PI / 4.0)));
    printf("test_from_axis_angle: PASSED\n");
}

static void test_from_axis_angle_zero() {
    /* Zero-length axis returns identity */
    void *axis = rt_vec3_new(0.0, 0.0, 0.0);
    void *q = rt_quat_from_axis_angle(axis, PI);
    assert(approx_eq(rt_quat_w(q), 1.0));
    printf("test_from_axis_angle_zero: PASSED\n");
}

static void test_from_euler() {
    /* Identity rotation */
    void *q = rt_quat_from_euler(0.0, 0.0, 0.0);
    assert(approx_eq(rt_quat_len(q), 1.0));
    assert(approx_eq(rt_quat_w(q), 1.0));

    /* Axis mapping must match Transform3D.SetEuler: pitch -> X, yaw -> Y, roll -> Z. */
    void *pitch_q = rt_quat_from_euler(PI / 2.0, 0.0, 0.0);
    assert(approx_eq(rt_quat_x(pitch_q), sin(PI / 4.0)));
    assert(approx_eq(rt_quat_y(pitch_q), 0.0));
    assert(approx_eq(rt_quat_z(pitch_q), 0.0));
    assert(approx_eq(rt_quat_w(pitch_q), cos(PI / 4.0)));

    void *yaw_q = rt_quat_from_euler(0.0, PI / 2.0, 0.0);
    assert(approx_eq(rt_quat_x(yaw_q), 0.0));
    assert(approx_eq(rt_quat_y(yaw_q), sin(PI / 4.0)));
    assert(approx_eq(rt_quat_z(yaw_q), 0.0));
    assert(approx_eq(rt_quat_w(yaw_q), cos(PI / 4.0)));

    void *roll_q = rt_quat_from_euler(0.0, 0.0, PI / 2.0);
    assert(approx_eq(rt_quat_x(roll_q), 0.0));
    assert(approx_eq(rt_quat_y(roll_q), 0.0));
    assert(approx_eq(rt_quat_z(roll_q), sin(PI / 4.0)));
    assert(approx_eq(rt_quat_w(roll_q), cos(PI / 4.0)));

    /* Yaw must match the equivalent axis-angle rotation exactly. */
    void *axis_y = rt_quat_from_axis_angle(rt_vec3_new(0.0, 1.0, 0.0), PI / 2.0);
    assert(approx_eq(rt_quat_x(yaw_q), rt_quat_x(axis_y)));
    assert(approx_eq(rt_quat_y(yaw_q), rt_quat_y(axis_y)));
    assert(approx_eq(rt_quat_z(yaw_q), rt_quat_z(axis_y)));
    assert(approx_eq(rt_quat_w(yaw_q), rt_quat_w(axis_y)));
    printf("test_from_euler: PASSED\n");
}

// ============================================================================
// Operations
// ============================================================================

static void test_mul_identity() {
    void *q = rt_quat_from_axis_angle(rt_vec3_new(1.0, 0.0, 0.0), PI / 3.0);
    void *id = rt_quat_identity();
    void *r = rt_quat_mul(q, id);
    assert(approx_eq(rt_quat_x(r), rt_quat_x(q)));
    assert(approx_eq(rt_quat_y(r), rt_quat_y(q)));
    assert(approx_eq(rt_quat_z(r), rt_quat_z(q)));
    assert(approx_eq(rt_quat_w(r), rt_quat_w(q)));
    printf("test_mul_identity: PASSED\n");
}

static void test_mul_inverse() {
    /* q * q^-1 = identity */
    void *q = rt_quat_from_axis_angle(rt_vec3_new(1.0, 0.0, 0.0), PI / 4.0);
    void *qi = rt_quat_inverse(q);
    void *r = rt_quat_mul(q, qi);
    assert(approx_eq(rt_quat_x(r), 0.0));
    assert(approx_eq(rt_quat_y(r), 0.0));
    assert(approx_eq(rt_quat_z(r), 0.0));
    assert(approx_eq(rt_quat_w(r), 1.0));
    printf("test_mul_inverse: PASSED\n");
}

// VDOC-206: Quat.Inverse must stay finite and nonzero for extreme finite norms.
// Forming |q|^2 = len*len overflowed for large norms (inverse collapsed to 0)
// and underflowed for tiny norms (inverse blew up to +/-Inf); the scaled
// (c/len)/len form avoids both.
static void test_inverse_extreme_norms() {
    // Very large norm: inverse X ~ -1e-308, finite and nonzero (was 0). Use a
    // relative bound since absolute EPSILON is meaningless at this scale.
    void *big = rt_quat_new(1e308, 0.0, 0.0, 0.0);
    void *big_inv = rt_quat_inverse(big);
    double bx = rt_quat_x(big_inv);
    assert(std::isfinite(bx) && bx < 0.0);
    assert(bx > -2e-308 && bx < -5e-309); // ~ -1e-308

    // Very small norm: inverse X ~ -1e308, finite (was -Inf).
    void *small = rt_quat_new(1e-308, 0.0, 0.0, 0.0);
    void *small_inv = rt_quat_inverse(small);
    double sx = rt_quat_x(small_inv);
    assert(std::isfinite(sx));
    assert(sx < -5e307 && sx > -1.6e308); // ~ -1e308

    // Sanity: a normal quaternion's inverse still satisfies q * q^-1 = identity.
    void *q2 = rt_quat_new(0.0, 0.0, 0.0, 2.0);
    void *qi = rt_quat_inverse(q2);
    assert(approx_eq(rt_quat_w(qi), 0.5));
    printf("test_inverse_extreme_norms: PASSED\n");
}

static void test_conjugate() {
    void *q = rt_quat_new(1.0, 2.0, 3.0, 4.0);
    void *c = rt_quat_conjugate(q);
    assert(approx_eq(rt_quat_x(c), -1.0));
    assert(approx_eq(rt_quat_y(c), -2.0));
    assert(approx_eq(rt_quat_z(c), -3.0));
    assert(approx_eq(rt_quat_w(c), 4.0));
    printf("test_conjugate: PASSED\n");
}

static void test_norm() {
    void *q = rt_quat_new(1.0, 2.0, 3.0, 4.0);
    void *n = rt_quat_norm(q);
    assert(approx_eq(rt_quat_len(n), 1.0));
    printf("test_norm: PASSED\n");
}

static void test_len() {
    void *q = rt_quat_new(1.0, 0.0, 0.0, 0.0);
    assert(approx_eq(rt_quat_len(q), 1.0));

    void *q2 = rt_quat_new(1.0, 2.0, 3.0, 4.0);
    double expected = sqrt(1.0 + 4.0 + 9.0 + 16.0);
    assert(approx_eq(rt_quat_len(q2), expected));
    printf("test_len: PASSED\n");
}

static void test_len_sq() {
    void *q = rt_quat_new(1.0, 2.0, 3.0, 4.0);
    assert(approx_eq(rt_quat_len_sq(q), 30.0));
    printf("test_len_sq: PASSED\n");
}

static void test_dot() {
    void *a = rt_quat_new(1.0, 0.0, 0.0, 0.0);
    void *b = rt_quat_new(0.0, 1.0, 0.0, 0.0);
    assert(approx_eq(rt_quat_dot(a, b), 0.0));

    void *c = rt_quat_new(1.0, 2.0, 3.0, 4.0);
    void *d = rt_quat_new(5.0, 6.0, 7.0, 8.0);
    /* 5 + 12 + 21 + 32 = 70 */
    assert(approx_eq(rt_quat_dot(c, d), 70.0));
    printf("test_dot: PASSED\n");
}

// ============================================================================
// Interpolation
// ============================================================================

static void test_slerp_endpoints() {
    void *a = rt_quat_identity();
    void *b = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), PI / 2.0);

    /* t=0 should give a */
    void *r0 = rt_quat_slerp(a, b, 0.0);
    assert(approx_eq(rt_quat_x(r0), rt_quat_x(a)));
    assert(approx_eq(rt_quat_y(r0), rt_quat_y(a)));
    assert(approx_eq(rt_quat_z(r0), rt_quat_z(a)));
    assert(approx_eq(rt_quat_w(r0), rt_quat_w(a)));

    /* t=1 should give b */
    void *r1 = rt_quat_slerp(a, b, 1.0);
    assert(approx_eq(rt_quat_x(r1), rt_quat_x(b)));
    assert(approx_eq(rt_quat_y(r1), rt_quat_y(b)));
    assert(approx_eq(rt_quat_z(r1), rt_quat_z(b)));
    assert(approx_eq(rt_quat_w(r1), rt_quat_w(b)));

    printf("test_slerp_endpoints: PASSED\n");
}

static void test_slerp_midpoint() {
    void *a = rt_quat_identity();
    void *b = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), PI / 2.0);

    /* Midpoint should be 45-degree rotation around Z */
    void *mid = rt_quat_slerp(a, b, 0.5);
    assert(approx_eq(rt_quat_len(mid), 1.0));

    /* The angle of the midpoint should be half of b's angle */
    double mid_angle = rt_quat_angle(mid);
    assert(approx_eq(mid_angle, PI / 4.0));
    printf("test_slerp_midpoint: PASSED\n");
}

static void test_lerp() {
    void *a = rt_quat_identity();
    void *b = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), PI / 4.0);

    /* Lerp result should be unit length (nlerp) */
    void *mid = rt_quat_lerp(a, b, 0.5);
    assert(approx_eq(rt_quat_len(mid), 1.0));
    printf("test_lerp: PASSED\n");
}

// ============================================================================
// Rotation
// ============================================================================

static void test_rotate_vec3() {
    /* 90 degrees around Z should rotate (1,0,0) to (0,1,0) */
    void *q = rt_quat_from_axis_angle(rt_vec3_new(0.0, 0.0, 1.0), PI / 2.0);
    void *v = rt_vec3_new(1.0, 0.0, 0.0);
    void *r = rt_quat_rotate_vec3(q, v);
    assert(approx_eq(rt_vec3_x(r), 0.0));
    assert(approx_eq(rt_vec3_y(r), 1.0));
    assert(approx_eq(rt_vec3_z(r), 0.0));
    printf("test_rotate_vec3: PASSED\n");
}

static void test_rotate_vec3_180() {
    /* 180 degrees around Y should rotate (1,0,0) to (-1,0,0) */
    void *q = rt_quat_from_axis_angle(rt_vec3_new(0.0, 1.0, 0.0), PI);
    void *v = rt_vec3_new(1.0, 0.0, 0.0);
    void *r = rt_quat_rotate_vec3(q, v);
    assert(approx_eq(rt_vec3_x(r), -1.0));
    assert(approx_eq(rt_vec3_y(r), 0.0));
    assert(approx_eq(rt_vec3_z(r), 0.0));
    printf("test_rotate_vec3_180: PASSED\n");
}

static void test_rotate_identity() {
    /* Identity rotation should not change the vector */
    void *q = rt_quat_identity();
    void *v = rt_vec3_new(3.0, 4.0, 5.0);
    void *r = rt_quat_rotate_vec3(q, v);
    assert(approx_eq(rt_vec3_x(r), 3.0));
    assert(approx_eq(rt_vec3_y(r), 4.0));
    assert(approx_eq(rt_vec3_z(r), 5.0));
    printf("test_rotate_identity: PASSED\n");
}

// ============================================================================
// Axis/Angle extraction
// ============================================================================

static void test_axis_angle_roundtrip() {
    void *axis = rt_vec3_new(0.0, 1.0, 0.0);
    double angle = PI / 3.0;
    void *q = rt_quat_from_axis_angle(axis, angle);

    double extracted_angle = rt_quat_angle(q);
    assert(approx_eq(extracted_angle, angle));

    void *extracted_axis = rt_quat_axis(q);
    assert(approx_eq(rt_vec3_x(extracted_axis), 0.0));
    assert(approx_eq(rt_vec3_y(extracted_axis), 1.0));
    assert(approx_eq(rt_vec3_z(extracted_axis), 0.0));
    printf("test_axis_angle_roundtrip: PASSED\n");
}

// ============================================================================
// ToMat4
// ============================================================================

static void test_to_mat4_identity() {
    void *q = rt_quat_identity();
    void *m = rt_quat_to_mat4(q);
    assert(m != nullptr);
    /* Diagonal should be 1, off-diagonal 0 for identity */
    assert(approx_eq(rt_mat4_get(m, 0, 0), 1.0));
    assert(approx_eq(rt_mat4_get(m, 1, 1), 1.0));
    assert(approx_eq(rt_mat4_get(m, 2, 2), 1.0));
    assert(approx_eq(rt_mat4_get(m, 3, 3), 1.0));
    assert(approx_eq(rt_mat4_get(m, 0, 1), 0.0));
    assert(approx_eq(rt_mat4_get(m, 0, 2), 0.0));
    printf("test_to_mat4_identity: PASSED\n");
}

// ============================================================================
// ToEuler / FromMat4 inverses (ADR 0227)
// ============================================================================

/// Compare two rotations up to quaternion double-cover sign.
static bool same_rotation(void *a, void *b) {
    double dot = rt_quat_x(a) * rt_quat_x(b) + rt_quat_y(a) * rt_quat_y(b) +
                 rt_quat_z(a) * rt_quat_z(b) + rt_quat_w(a) * rt_quat_w(b);
    return fabs(fabs(dot) - 1.0) < 1e-6;
}

static void test_to_euler_roundtrip_sweep() {
    /* FromEuler(ToEuler(q)) must recover q (up to sign) across a dense
     * pitch/yaw/roll sweep including values near the gimbal poles. */
    for (int p = -2; p <= 2; ++p) {
        for (int y = -2; y <= 2; ++y) {
            for (int r = -2; r <= 2; ++r) {
                double pitch = p * (PI / 5.0);
                double yaw = y * (PI / 5.0);
                double roll = r * (PI / 5.0);
                void *q = rt_quat_from_euler(pitch, yaw, roll);
                void *euler = rt_quat_to_euler(q);
                assert(euler != nullptr);
                void *back = rt_quat_from_euler(
                    rt_vec3_x(euler), rt_vec3_y(euler), rt_vec3_z(euler));
                assert(same_rotation(q, back));
            }
        }
    }
    printf("test_to_euler_roundtrip_sweep: PASSED\n");
}

static void test_to_euler_gimbal_poles() {
    /* Exact poles: yaw = +/-pi/2 with a twist shared between pitch and roll. */
    for (int sign = -1; sign <= 1; sign += 2) {
        for (int t = -3; t <= 3; ++t) {
            double twist = t * (PI / 7.0);
            void *q = rt_quat_from_euler(twist, sign * (PI / 2.0), 0.0);
            void *euler = rt_quat_to_euler(q);
            assert(euler != nullptr);
            void *back = rt_quat_from_euler(
                rt_vec3_x(euler), rt_vec3_y(euler), rt_vec3_z(euler));
            assert(same_rotation(q, back));
        }
    }
    printf("test_to_euler_gimbal_poles: PASSED\n");
}

static void test_to_euler_matches_reference_decomposition() {
    /* The exact decomposition Zanna Studio shipped before ADR 0227. */
    void *q = rt_quat_from_euler(0.3, -0.7, 1.1);
    double x = rt_quat_x(q);
    double y = rt_quat_y(q);
    double z = rt_quat_z(q);
    double w = rt_quat_w(q);
    double ref_pitch = atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
    double ref_yaw = asin(2.0 * (w * y - z * x));
    double ref_roll = atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
    void *euler = rt_quat_to_euler(q);
    assert(approx_eq(rt_vec3_x(euler), ref_pitch));
    assert(approx_eq(rt_vec3_y(euler), ref_yaw));
    assert(approx_eq(rt_vec3_z(euler), ref_roll));
    printf("test_to_euler_matches_reference_decomposition: PASSED\n");
}

static void test_from_mat4_roundtrip_sweep() {
    /* FromMat4(ToMat4(q)) must recover q (up to sign) for every trace branch. */
    for (int p = -2; p <= 2; ++p) {
        for (int y = -2; y <= 2; ++y) {
            for (int r = -2; r <= 2; ++r) {
                void *q = rt_quat_from_euler(
                    p * (PI / 3.0), y * (PI / 3.0), r * (PI / 3.0));
                void *m = rt_quat_to_mat4(q);
                void *back = rt_quat_from_mat4(m);
                assert(back != nullptr);
                assert(same_rotation(q, back));
            }
        }
    }
    printf("test_from_mat4_roundtrip_sweep: PASSED\n");
}

static void test_from_mat4_tolerates_scale() {
    /* Column-normalization: a uniformly scaled rotation decomposes cleanly. */
    void *q = rt_quat_from_euler(0.4, 0.9, -0.2);
    void *m = rt_quat_to_mat4(q);
    void *scale = rt_mat4_scale(3.0, 3.0, 3.0);
    void *scaled = rt_mat4_mul(m, scale);
    void *back = rt_quat_from_mat4(scaled);
    assert(same_rotation(q, back));
    printf("test_from_mat4_tolerates_scale: PASSED\n");
}

static void test_from_mat4_degenerate_is_identity() {
    void *zero = rt_mat4_scale(0.0, 0.0, 0.0);
    void *q = rt_quat_from_mat4(zero);
    assert(approx_eq(rt_quat_x(q), 0.0));
    assert(approx_eq(rt_quat_y(q), 0.0));
    assert(approx_eq(rt_quat_z(q), 0.0));
    assert(approx_eq(rt_quat_w(q), 1.0));
    printf("test_from_mat4_degenerate_is_identity: PASSED\n");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== Zanna.Quat Tests ===\n\n");

    /* Constructors */
    test_new();
    test_identity();
    test_from_axis_angle();
    test_from_axis_angle_zero();
    test_from_euler();

    /* Operations */
    test_mul_identity();
    test_mul_inverse();
    test_inverse_extreme_norms();
    test_conjugate();
    test_norm();
    test_len();
    test_len_sq();
    test_dot();

    /* Interpolation */
    test_slerp_endpoints();
    test_slerp_midpoint();
    test_lerp();

    /* Rotation */
    test_rotate_vec3();
    test_rotate_vec3_180();
    test_rotate_identity();

    /* Axis/Angle */
    test_axis_angle_roundtrip();

    /* ToMat4 */
    test_to_mat4_identity();

    /* ToEuler / FromMat4 inverses (ADR 0227) */
    test_to_euler_roundtrip_sweep();
    test_to_euler_gimbal_poles();
    test_to_euler_matches_reference_decomposition();
    test_from_mat4_roundtrip_sweep();
    test_from_mat4_tolerates_scale();
    test_from_mat4_degenerate_is_identity();

    printf("\nAll Quat tests passed!\n");
    return 0;
}
