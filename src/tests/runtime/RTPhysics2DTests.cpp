//===----------------------------------------------------------------------===//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTPhysics2DTests.cpp
// Purpose: Regression tests for Physics2D worlds, bodies, collision, contacts,
//   numerical limits, handle validation, and analytic projectiles.
//
// Key invariants:
//   - A body belongs to at most one world and all retained arrays remain packed.
//   - Shape, mass, force, collision, and projectile math stays finite at the
//     supported numeric limits.
//   - Same-class undersized and uninitialized full-size runtime payloads are
//     rejected before implementation fields are accessed.
//
// Ownership/Lifetime:
//   - Tests retain caller references and release every explicitly allocated
//     runtime object after the owning world has finished using it.
//
// Links: src/runtime/graphics/2d/rt_physics2d.c,
//   src/runtime/graphics/2d/rt_physics2d_collision.c,
//   src/runtime/graphics/2d/rt_physics2d_joint.c
//
// RTPhysics2DTests.cpp - Tests for rt_physics2d (2D physics engine)
//===----------------------------------------------------------------------===//

#include <cmath>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" {
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_physics2d.h"
#include "rt_physics2d_internal.h"
#include "rt_physics2d_joint.h"
}

//=============================================================================
// Trap infrastructure (same pattern as RTBinaryBufferTests)
//=============================================================================

namespace {
static jmp_buf g_trap_jmp;
static bool g_trap_expected = false;
} // namespace

extern "C" void vm_trap(const char *msg) {
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    fprintf(stderr, "TRAP: %s\n", msg);
    rt_abort(msg);
}

#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            (void)(expr);                                                                          \
            fprintf(stderr, "FAIL [%s:%d]: Expected trap did not fire\n", __FILE__, __LINE__);     \
            tests_run++;                                                                           \
        } else {                                                                                   \
            tests_run++;                                                                           \
            tests_passed++;                                                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

static int tests_run = 0;
static int tests_passed = 0;
static int g_body_finalizer_calls = 0;

static const double EPSILON = 1e-6;

#define ASSERT(cond, msg)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg);                        \
        } else {                                                                                   \
            tests_passed++;                                                                        \
        }                                                                                          \
    } while (0)

#define ASSERT_NEAR(a, b, msg) ASSERT(fabs((a) - (b)) < EPSILON, msg)

static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

extern "C" void test_body_finalizer(void *) {
    g_body_finalizer_calls++;
}

//=============================================================================
// World tests
//=============================================================================

static void test_world_new() {
    void *world = rt_physics2d_world_new(0.0, 9.8);
    ASSERT(world != NULL, "world_new returns non-null");
    ASSERT(rt_physics2d_world_body_count(world) == 0, "new world has 0 bodies");
    rt_obj_release_check0(world);
}

static void test_world_add_remove() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);

    rt_physics2d_world_add(world, body);
    ASSERT(rt_physics2d_world_body_count(world) == 1, "1 body after add");

    rt_physics2d_world_remove(world, body);
    ASSERT(rt_physics2d_world_body_count(world) == 0, "0 bodies after remove");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

static void test_world_add_multiple() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *b1 = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    void *b2 = rt_physics2d_body_new(50, 0, 10, 10, 1.0);
    void *b3 = rt_physics2d_body_new(100, 0, 10, 10, 1.0);

    rt_physics2d_world_add(world, b1);
    rt_physics2d_world_add(world, b2);
    rt_physics2d_world_add(world, b3);
    ASSERT(rt_physics2d_world_body_count(world) == 3, "3 bodies after adds");

    rt_obj_release_check0(b1);
    rt_obj_release_check0(b2);
    rt_obj_release_check0(b3);
    rt_obj_release_check0(world);
}

static void test_duplicate_body_add_ignored() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);

    rt_physics2d_world_add(world, body);
    rt_physics2d_world_add(world, body);
    ASSERT(rt_physics2d_world_body_count(world) == 1, "duplicate body add is ignored");

    release_obj(body);
    release_obj(world);
}

//=============================================================================
// Body tests
//=============================================================================

static void test_body_new() {
    void *body = rt_physics2d_body_new(10.0, 20.0, 30.0, 40.0, 5.0);
    ASSERT(body != NULL, "body_new returns non-null");
    ASSERT_NEAR(rt_physics2d_body_x(body), 10.0, "x = 10");
    ASSERT_NEAR(rt_physics2d_body_y(body), 20.0, "y = 20");
    ASSERT_NEAR(rt_physics2d_body_w(body), 30.0, "w = 30");
    ASSERT_NEAR(rt_physics2d_body_h(body), 40.0, "h = 40");
    ASSERT_NEAR(rt_physics2d_body_vx(body), 0.0, "vx = 0");
    ASSERT_NEAR(rt_physics2d_body_vy(body), 0.0, "vy = 0");
    ASSERT_NEAR(rt_physics2d_body_mass(body), 5.0, "mass = 5");
    ASSERT(rt_physics2d_body_is_static(body) == 0, "not static");
    rt_obj_release_check0(body);
}

static void test_body_static() {
    void *body = rt_physics2d_body_new(0, 0, 100, 10, 0.0);
    ASSERT(rt_physics2d_body_is_static(body) == 1, "mass=0 is static");
    ASSERT_NEAR(rt_physics2d_body_mass(body), 0.0, "mass = 0");
    rt_obj_release_check0(body);
}

static void test_body_set_pos() {
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    rt_physics2d_body_set_pos(body, 42.0, 99.0);
    ASSERT_NEAR(rt_physics2d_body_x(body), 42.0, "x after set_pos");
    ASSERT_NEAR(rt_physics2d_body_y(body), 99.0, "y after set_pos");
    ASSERT_NEAR(rt_physics2d_body_prev_x(body), 42.0, "prev_x after set_pos");
    ASSERT_NEAR(rt_physics2d_body_prev_y(body), 99.0, "prev_y after set_pos");
    rt_obj_release_check0(body);
}

static void test_body_set_vel() {
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    rt_physics2d_body_set_vel(body, 5.0, -3.0);
    ASSERT_NEAR(rt_physics2d_body_vx(body), 5.0, "vx after set_vel");
    ASSERT_NEAR(rt_physics2d_body_vy(body), -3.0, "vy after set_vel");
    rt_obj_release_check0(body);
}

static void test_body_restitution_friction() {
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    // Defaults
    ASSERT_NEAR(rt_physics2d_body_restitution(body), 0.5, "default restitution = 0.5");
    ASSERT_NEAR(rt_physics2d_body_friction(body), 0.3, "default friction = 0.3");

    rt_physics2d_body_set_restitution(body, 0.9);
    rt_physics2d_body_set_friction(body, 0.1);
    ASSERT_NEAR(rt_physics2d_body_restitution(body), 0.9, "restitution after set");
    ASSERT_NEAR(rt_physics2d_body_friction(body), 0.1, "friction after set");
    rt_obj_release_check0(body);
}

static void test_body_inputs_sanitized() {
    double nan = std::nan("");
    void *body = rt_physics2d_body_new(nan, nan, -2.0, 0.0, nan);

    ASSERT_NEAR(rt_physics2d_body_x(body), 0.0, "NaN x sanitized to 0");
    ASSERT_NEAR(rt_physics2d_body_y(body), 0.0, "NaN y sanitized to 0");
    ASSERT_NEAR(rt_physics2d_body_w(body), 1.0, "invalid width sanitized to 1");
    ASSERT_NEAR(rt_physics2d_body_h(body), 1.0, "invalid height sanitized to 1");
    ASSERT(rt_physics2d_body_is_static(body) == 1, "invalid mass becomes static");

    release_obj(body);
}

static void test_static_set_vel_ignored() {
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 0.0);
    rt_physics2d_body_set_vel(body, 100.0, -25.0);
    ASSERT_NEAR(rt_physics2d_body_vx(body), 0.0, "static body ignores set_vel vx");
    ASSERT_NEAR(rt_physics2d_body_vy(body), 0.0, "static body ignores set_vel vy");
    release_obj(body);
}

static void test_restitution_friction_clamped() {
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    rt_physics2d_body_set_restitution(body, 2.0);
    rt_physics2d_body_set_friction(body, -1.0);
    ASSERT_NEAR(rt_physics2d_body_restitution(body), 1.0, "restitution clamps high to 1");
    ASSERT_NEAR(rt_physics2d_body_friction(body), 0.0, "friction clamps low to 0");

    rt_physics2d_body_set_restitution(body, std::nan(""));
    rt_physics2d_body_set_friction(body, std::nan(""));
    ASSERT_NEAR(rt_physics2d_body_restitution(body), 0.0, "NaN restitution clamps to 0");
    ASSERT_NEAR(rt_physics2d_body_friction(body), 0.0, "NaN friction clamps to 0");
    release_obj(body);
}

//=============================================================================
// Integration tests
//=============================================================================

static void test_gravity_integration() {
    void *world = rt_physics2d_world_new(0.0, 10.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    rt_physics2d_world_add(world, body);

    // Step 1 second
    rt_physics2d_world_step(world, 1.0);

    // After 1s with gravity 10: vy = 10, y = 10
    ASSERT_NEAR(rt_physics2d_body_vy(body), 10.0, "vy = 10 after 1s gravity");
    ASSERT_NEAR(rt_physics2d_body_y(body), 10.0, "y = 10 after 1s gravity");
    ASSERT_NEAR(rt_physics2d_body_x(body), 0.0, "x unchanged");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

static void test_velocity_integration() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    rt_physics2d_body_set_vel(body, 100.0, 50.0);
    rt_physics2d_world_add(world, body);

    rt_physics2d_world_step(world, 0.5);

    ASSERT_NEAR(rt_physics2d_body_x(body), 50.0, "x = 50 after 0.5s at vx=100");
    ASSERT_NEAR(rt_physics2d_body_y(body), 25.0, "y = 25 after 0.5s at vy=50");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

static void test_static_body_no_gravity() {
    void *world = rt_physics2d_world_new(0.0, 100.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 0.0); // static
    rt_physics2d_world_add(world, body);

    rt_physics2d_world_step(world, 1.0);

    ASSERT_NEAR(rt_physics2d_body_x(body), 0.0, "static x unchanged");
    ASSERT_NEAR(rt_physics2d_body_y(body), 0.0, "static y unchanged");
    ASSERT_NEAR(rt_physics2d_body_vy(body), 0.0, "static vy unchanged");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

static void test_force_application() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 2.0);
    rt_physics2d_world_add(world, body);

    // Apply force of 20 to mass-2 body => acceleration = 10
    rt_physics2d_body_apply_force(body, 20.0, 0.0);
    rt_physics2d_world_step(world, 1.0);

    ASSERT_NEAR(rt_physics2d_body_vx(body), 10.0, "vx = F/m * t = 10");
    ASSERT_NEAR(rt_physics2d_body_x(body), 10.0, "x = v*t = 10");

    // Force should be cleared after step
    rt_physics2d_world_step(world, 1.0);
    ASSERT_NEAR(rt_physics2d_body_vx(body), 10.0, "vx unchanged (no new force)");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

static void test_impulse_application() {
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 2.0);

    // Impulse of 10 on mass-2 body => dv = impulse * inv_mass = 10 * 0.5 = 5
    rt_physics2d_body_apply_impulse(body, 10.0, 0.0);
    ASSERT_NEAR(rt_physics2d_body_vx(body), 5.0, "impulse changes velocity instantly");

    // Static body ignores impulse
    void *staticBody = rt_physics2d_body_new(0, 0, 10, 10, 0.0);
    rt_physics2d_body_apply_impulse(staticBody, 100.0, 100.0);
    ASSERT_NEAR(rt_physics2d_body_vx(staticBody), 0.0, "static ignores impulse");

    rt_obj_release_check0(body);
    rt_obj_release_check0(staticBody);
}

//=============================================================================
// Collision tests
//=============================================================================

static void test_collision_detection() {
    void *world = rt_physics2d_world_new(0.0, 0.0);

    // Two overlapping bodies: overlap of 5 units on x-axis
    void *a = rt_physics2d_body_new(0, 0, 20, 20, 1.0);
    void *b = rt_physics2d_body_new(15, 0, 20, 20, 1.0);

    // Moving toward each other
    rt_physics2d_body_set_vel(a, 10.0, 0.0);
    rt_physics2d_body_set_vel(b, -10.0, 0.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);

    // After step, collision should modify velocities
    double va_before = rt_physics2d_body_vx(a);
    double vb_before = rt_physics2d_body_vx(b);
    /// @brief Rt_physics2d_world_step.
    rt_physics2d_world_step(world, 0.001); // tiny dt to minimize integration drift

    double va = rt_physics2d_body_vx(a);
    double vb = rt_physics2d_body_vx(b);
    // Bodies were overlapping and moving toward each other,
    // so collision resolution should change their velocities
    ASSERT(fabs(va - va_before) > EPSILON || fabs(vb - vb_before) > EPSILON,
           "collision changed at least one velocity");

    rt_obj_release_check0(a);
    rt_obj_release_check0(b);
    rt_obj_release_check0(world);
}

static void test_no_collision_separated() {
    void *world = rt_physics2d_world_new(0.0, 0.0);

    void *a = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    void *b = rt_physics2d_body_new(100, 0, 10, 10, 1.0);

    rt_physics2d_body_set_vel(a, 5.0, 0.0);
    rt_physics2d_body_set_vel(b, -5.0, 0.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);

    rt_physics2d_world_step(world, 0.016);

    // Bodies are far apart, velocities should be unchanged
    ASSERT_NEAR(rt_physics2d_body_vx(a), 5.0, "separated A vx unchanged");
    ASSERT_NEAR(rt_physics2d_body_vx(b), -5.0, "separated B vx unchanged");

    rt_obj_release_check0(a);
    rt_obj_release_check0(b);
    rt_obj_release_check0(world);
}

static void test_collision_with_static() {
    void *world = rt_physics2d_world_new(0.0, 0.0);

    // Dynamic body overlapping static body
    void *dynamic = rt_physics2d_body_new(0, 0, 20, 20, 1.0);
    void *wall = rt_physics2d_body_new(15, 0, 20, 20, 0.0); // static

    rt_physics2d_body_set_vel(dynamic, 10.0, 0.0);
    rt_physics2d_body_set_restitution(dynamic, 1.0);
    rt_physics2d_body_set_restitution(wall, 1.0);

    rt_physics2d_world_add(world, dynamic);
    rt_physics2d_world_add(world, wall);

    rt_physics2d_world_step(world, 0.016);

    // Static body should remain in place
    ASSERT_NEAR(rt_physics2d_body_x(wall), 15.0, "static wall x unchanged");
    ASSERT_NEAR(rt_physics2d_body_vx(wall), 0.0, "static wall vx = 0");

    rt_obj_release_check0(dynamic);
    rt_obj_release_check0(wall);
    rt_obj_release_check0(world);
}

static void test_set_gravity() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    rt_physics2d_world_add(world, body);

    // No gravity initially
    rt_physics2d_world_step(world, 1.0);
    ASSERT_NEAR(rt_physics2d_body_vy(body), 0.0, "no gravity, vy=0");

    // Set gravity
    rt_physics2d_world_set_gravity(world, 0.0, 5.0);
    rt_physics2d_world_step(world, 1.0);
    ASSERT_NEAR(rt_physics2d_body_vy(body), 5.0, "vy=5 after gravity set");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

static void test_invalid_dt_noop() {
    void *world = rt_physics2d_world_new(0.0, 10.0);
    void *body = rt_physics2d_body_new(5.0, 5.0, 10.0, 10.0, 1.0);
    rt_physics2d_world_add(world, body);

    rt_physics2d_world_step(world, std::nan(""));
    ASSERT_NEAR(rt_physics2d_body_x(body), 5.0, "x unchanged with NaN dt");
    ASSERT_NEAR(rt_physics2d_body_y(body), 5.0, "y unchanged with NaN dt");
    ASSERT_NEAR(rt_physics2d_body_vy(body), 0.0, "velocity unchanged with NaN dt");

    release_obj(body);
    release_obj(world);
}

static void test_large_dt_is_deferred_instead_of_discarded() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 1.0, 1.0, 1.0);
    rt_physics2d_body_set_vel(body, 1.0, 0.0);
    rt_physics2d_world_add(world, body);

    rt_physics2d_world_step(world, 10.0);
    ASSERT_NEAR(rt_physics2d_body_x(body), 8.0, "one Step consumes its bounded eight seconds");
    ASSERT_NEAR(static_cast<rt_world_impl *>(world)->pending_dt,
                2.0,
                "excess elapsed time remains pending");

    rt_physics2d_world_step(world, 1.0);
    ASSERT_NEAR(rt_physics2d_body_x(body), 11.0, "next positive Step consumes deferred time");
    ASSERT_NEAR(
        static_cast<rt_world_impl *>(world)->pending_dt, 0.0, "deferred time drains exactly");

    release_obj(body);
    release_obj(world);
}

static void test_zero_gravity_body_sleeps_and_external_force_wakes_it() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(4.0, 7.0, 1.0, 1.0, 1.0);
    rt_physics2d_world_add(world, body);

    rt_physics2d_world_step(world, 0.5);
    auto *impl = static_cast<rt_body_impl *>(body);
    ASSERT(impl->is_sleeping == 1, "stationary zero-gravity body enters safe sleep");
    rt_physics2d_world_step(world, 0.5);
    ASSERT_NEAR(rt_physics2d_body_x(body), 4.0, "sleeping body skips position integration");

    rt_physics2d_body_apply_force(body, 2.0, 0.0);
    ASSERT(impl->is_sleeping == 0, "nonzero force wakes sleeping body immediately");
    rt_physics2d_world_step(world, 0.5);
    ASSERT(rt_physics2d_body_x(body) > 4.0, "woken body integrates the applied force");

    rt_physics2d_world_set_gravity(world, 0.0, 1.0);
    ASSERT(impl->is_sleeping == 0, "gravity changes keep dynamic bodies awake");

    release_obj(body);
    release_obj(world);
}

static void test_separating_overlap_still_corrected() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    void *b = rt_physics2d_body_new(5, 0, 10, 10, 1.0);
    rt_physics2d_body_set_vel(a, -10.0, 0.0);
    rt_physics2d_body_set_vel(b, 10.0, 0.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);
    rt_physics2d_world_step(world, 0.001);

    ASSERT(rt_physics2d_body_x(b) - rt_physics2d_body_x(a) > 5.5,
           "separating overlap still receives positional correction");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_circle_body_collides_with_aabb() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *circle = rt_physics2d_circle_body_new(0.0, 0.0, 5.0, 1.0);
    void *wall = rt_physics2d_body_new(4.0, -5.0, 10.0, 10.0, 0.0);
    rt_physics2d_body_set_vel(circle, 10.0, 0.0);

    rt_physics2d_world_add(world, circle);
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_step(world, 0.001);

    ASSERT(rt_physics2d_body_vx(circle) < 10.0 - EPSILON,
           "circle-vs-AABB overlap changes velocity");

    release_obj(circle);
    release_obj(wall);
    release_obj(world);
}

static void test_swept_aabb_hits_thin_wall() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    void *wall = rt_physics2d_body_new(50.0, 0.0, 1.0, 10.0, 0.0);
    rt_physics2d_body_set_vel(body, 1000.0, 0.0);

    rt_physics2d_world_add(world, body);
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_step(world, 0.1);

    ASSERT(rt_physics2d_body_x(body) <= 40.0 + EPSILON,
           "swept AABB collision stops at thin wall contact");
    ASSERT(rt_physics2d_body_vx(body) < 0.0, "swept AABB collision applies bounce impulse");

    release_obj(body);
    release_obj(wall);
    release_obj(world);
}

static void test_swept_circle_hits_thin_wall() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *circle = rt_physics2d_circle_body_new(0.0, 5.0, 5.0, 1.0);
    void *wall = rt_physics2d_body_new(50.0, 0.0, 1.0, 10.0, 0.0);
    rt_physics2d_body_set_vel(circle, 1000.0, 0.0);

    rt_physics2d_world_add(world, circle);
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_step(world, 0.1);

    ASSERT(rt_physics2d_body_x(circle) <= 45.0 + EPSILON,
           "swept circle collision catches thin wall");
    ASSERT(rt_physics2d_body_vx(circle) < 0.0, "swept circle collision applies bounce impulse");

    release_obj(circle);
    release_obj(wall);
    release_obj(world);
}

static void test_swept_circle_misses_near_diagonal_target() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *circle = rt_physics2d_circle_body_new(0.0, 0.0, 5.0, 1.0);
    void *target = rt_physics2d_circle_body_new(60.0, 40.0, 5.0, 0.0);
    rt_physics2d_body_set_vel(circle, 1000.0, 1000.0);

    rt_physics2d_world_add(world, circle);
    rt_physics2d_world_add(world, target);
    rt_physics2d_world_step(world, 0.1);

    ASSERT_NEAR(rt_physics2d_body_vx(circle), 1000.0, "swept circle miss leaves vx unchanged");
    ASSERT_NEAR(rt_physics2d_body_vy(circle), 1000.0, "swept circle miss leaves vy unchanged");
    ASSERT(rt_physics2d_world_contact_count(world) == 0, "swept circle miss records no contact");

    release_obj(circle);
    release_obj(target);
    release_obj(world);
}

static void test_circle_inside_aabb_resolves_outward() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *circle = rt_physics2d_circle_body_new(1.0, 5.0, 2.0, 1.0);
    void *wall = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 0.0);

    rt_physics2d_world_add(world, circle);
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_step(world, 0.001);

    ASSERT(rt_physics2d_body_x(circle) < 1.0,
           "circle center inside AABB resolves toward nearest exit side");

    release_obj(circle);
    release_obj(wall);
    release_obj(world);
}

static void test_circle_radius_allows_subunit_values() {
    void *circle = rt_physics2d_circle_body_new(0.0, 0.0, 0.25, 1.0);
    ASSERT_NEAR(rt_physics2d_body_radius(circle), 0.25, "circle radius preserves subunit value");
    release_obj(circle);
}

static void test_circle_aabb_tangent_is_not_overlap() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *circle = rt_physics2d_circle_body_new(0.0, 5.0, 5.0, 1.0);
    void *wall = rt_physics2d_body_new(5.0, 0.0, 10.0, 10.0, 0.0);

    rt_physics2d_world_add(world, circle);
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_step(world, 0.001);

    ASSERT(rt_physics2d_world_contact_count(world) == 0, "circle-AABB tangency is not overlap");

    release_obj(circle);
    release_obj(wall);
    release_obj(world);
}

static void test_world_records_step_contacts() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    void *b = rt_physics2d_body_new(5.0, 0.0, 10.0, 10.0, 1.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);
    rt_physics2d_world_step(world, 0.001);

    ASSERT(rt_physics2d_world_contact_count(world) >= 1, "overlap records at least one contact");
    ASSERT(rt_physics2d_world_contact_body_a(world, 0) == a, "contact body A is exposed");
    ASSERT(rt_physics2d_world_contact_body_b(world, 0) == b, "contact body B is exposed");
    ASSERT(std::isfinite(rt_physics2d_world_contact_nx(world, 0)), "contact nx is finite");
    ASSERT(std::isfinite(rt_physics2d_world_contact_ny(world, 0)), "contact ny is finite");
    ASSERT(rt_physics2d_world_contact_depth(world, 0) > 0.0, "overlap contact has depth");

    ASSERT(rt_physics2d_world_contact_body_a(world, -1) == NULL, "negative contact index is NULL");
    ASSERT(rt_physics2d_world_contact_body_b(world, 999) == NULL, "large contact index is NULL");
    ASSERT(rt_physics2d_world_contact_depth(world, 999) == 0.0, "invalid contact depth is zero");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_world_deduplicates_contacts_across_substeps() {
    void *world = rt_physics2d_world_new(0.0, 1.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    void *floor = rt_physics2d_body_new(0.0, 10.0, 10.0, 10.0, 0.0);
    rt_physics2d_body_set_restitution(body, 0.0);
    rt_physics2d_body_set_restitution(floor, 0.0);
    rt_physics2d_world_add(world, body);
    rt_physics2d_world_add(world, floor);

    rt_physics2d_world_step(world, 8.0);
    ASSERT(rt_physics2d_world_contact_count(world) == 1,
           "persistent pair has one public-step contact across eight substeps");
    ASSERT(rt_physics2d_world_contact_body_a(world, 0) == body,
           "deduplicated contact preserves body A");
    ASSERT(rt_physics2d_world_contact_body_b(world, 0) == floor,
           "deduplicated contact preserves body B");

    release_obj(body);
    release_obj(floor);
    release_obj(world);
}

static void test_world_clears_contacts_on_noop_step() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    void *b = rt_physics2d_body_new(5.0, 0.0, 10.0, 10.0, 1.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);
    rt_physics2d_world_step(world, 0.001);
    ASSERT(rt_physics2d_world_contact_count(world) > 0, "setup records contact");

    rt_physics2d_world_step(world, 0.0);
    ASSERT(rt_physics2d_world_contact_count(world) == 0, "zero-dt step clears old contacts");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_world_remove_clears_contacts() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    void *b = rt_physics2d_body_new(5.0, 0.0, 10.0, 10.0, 1.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);
    rt_physics2d_world_step(world, 0.001);
    ASSERT(rt_physics2d_world_contact_count(world) > 0, "setup records removable contact");

    rt_physics2d_world_remove(world, b);
    ASSERT(rt_physics2d_world_contact_count(world) == 0, "remove clears old contacts");
    ASSERT(rt_physics2d_world_contact_body_a(world, 0) == NULL, "removed contact body A hidden");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_world_contact_storage_grows_past_default_capacity() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    const int total = PH_MAX_CONTACTS + 10;
    void *bodies[PH_MAX_CONTACTS + 11] = {};
    for (int i = 0; i <= total; ++i) {
        bodies[i] = rt_physics2d_body_new((double)i, 0.0, 1.0, 1.0, 1.0);
        rt_physics2d_world_add(world, bodies[i]);
    }
    for (int i = 1; i <= total; i++) {
        world_record_contact((rt_world_impl *)world,
                             (rt_body_impl *)bodies[0],
                             (rt_body_impl *)bodies[i],
                             1.0,
                             0.0,
                             1.0);
    }
    ASSERT(rt_physics2d_world_contact_count(world) == total,
           "contact storage grows past PH_MAX_CONTACTS");
    ASSERT(rt_physics2d_world_contact_overflowed(world) == 0,
           "successful contact growth does not report overflow");

    rt_physics2d_world_step(world, 0.0);
    ASSERT(rt_physics2d_world_contact_count(world) == 0, "clear removes grown contacts");
    ASSERT(rt_physics2d_world_contact_overflowed(world) == 0, "clear resets overflow flag");

    for (int i = 0; i <= total; ++i)
        release_obj(bodies[i]);
    release_obj(world);
}

static void test_extreme_forces_are_sanitized() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    rt_physics2d_world_add(world, body);

    double huge = std::numeric_limits<double>::max() / 4.0;
    rt_physics2d_body_apply_impulse(body, huge, -huge);
    rt_physics2d_body_apply_force(body, huge, huge);
    rt_physics2d_world_step(world, huge);

    ASSERT(std::isfinite(rt_physics2d_body_x(body)), "extreme force leaves finite x");
    ASSERT(std::isfinite(rt_physics2d_body_y(body)), "extreme force leaves finite y");
    ASSERT(std::isfinite(rt_physics2d_body_vx(body)), "extreme force leaves finite vx");
    ASSERT(std::isfinite(rt_physics2d_body_vy(body)), "extreme force leaves finite vy");

    release_obj(body);
    release_obj(world);
}

//=============================================================================
// Null safety
//=============================================================================

static void test_null_safety() {
    // World functions
    ASSERT(rt_physics2d_world_body_count(NULL) == 0, "null world count = 0");
    /// @brief Rt_physics2d_world_step.
    rt_physics2d_world_step(NULL, 1.0);         // should not crash
                                                /// @brief Rt_physics2d_world_add.
    rt_physics2d_world_add(NULL, NULL);         // should not crash
                                                /// @brief Rt_physics2d_world_remove.
    rt_physics2d_world_remove(NULL, NULL);      // should not crash
                                                /// @brief Rt_physics2d_world_set_gravity.
    rt_physics2d_world_set_gravity(NULL, 0, 0); // should not crash

    // Body functions
    ASSERT_NEAR(rt_physics2d_body_x(NULL), 0.0, "null body x = 0");
    ASSERT_NEAR(rt_physics2d_body_y(NULL), 0.0, "null body y = 0");
    ASSERT_NEAR(rt_physics2d_body_vx(NULL), 0.0, "null body vx = 0");
    ASSERT_NEAR(rt_physics2d_body_vy(NULL), 0.0, "null body vy = 0");
    ASSERT_NEAR(rt_physics2d_body_mass(NULL), 0.0, "null body mass = 0");
    /// @brief Rt_physics2d_body_set_pos.
    rt_physics2d_body_set_pos(NULL, 0, 0);       // should not crash
                                                 /// @brief Rt_physics2d_body_set_vel.
    rt_physics2d_body_set_vel(NULL, 0, 0);       // should not crash
                                                 /// @brief Rt_physics2d_body_apply_force.
    rt_physics2d_body_apply_force(NULL, 0, 0);   // should not crash
                                                 /// @brief Rt_physics2d_body_apply_impulse.
    rt_physics2d_body_apply_impulse(NULL, 0, 0); // should not crash

    tests_run++;
    tests_passed++; // If we get here, null safety passed
}

static void test_wrong_handle_traps() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, 1.0);
    void *fake = rt_obj_new_i64(0, 8);

    EXPECT_TRAP(rt_physics2d_world_add(world, fake));
    EXPECT_TRAP(rt_physics2d_distance_joint_new(body, fake, 10.0));
    EXPECT_TRAP(rt_physics2d_world_add_joint(world, fake));
    EXPECT_TRAP(rt_physics2d_body_radius(fake));

    release_obj(fake);
    release_obj(body);
    release_obj(world);
}

static void test_zero_dt() {
    void *world = rt_physics2d_world_new(0.0, 10.0);
    void *body = rt_physics2d_body_new(5.0, 5.0, 10.0, 10.0, 1.0);
    rt_physics2d_world_add(world, body);

    /// @brief Rt_physics2d_world_step.
    rt_physics2d_world_step(world, 0.0); // zero dt should be no-op
    ASSERT_NEAR(rt_physics2d_body_x(body), 5.0, "x unchanged with dt=0");
    ASSERT_NEAR(rt_physics2d_body_y(body), 5.0, "y unchanged with dt=0");

    /// @brief Rt_physics2d_world_step.
    rt_physics2d_world_step(world, -1.0); // negative dt should be no-op
    ASSERT_NEAR(rt_physics2d_body_x(body), 5.0, "x unchanged with dt<0");

    rt_obj_release_check0(body);
    rt_obj_release_check0(world);
}

//=============================================================================
// GAME-H-6: collision_mask default should include bit 31 (0xFFFFFFFF)
//=============================================================================

static void test_collision_mask_default_full() {
    // Default mask must cover every 64-bit layer.
    void *body = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    int64_t mask = rt_physics2d_body_collision_mask(body);
    ASSERT((mask & (int64_t)0x80000000LL) != 0, "default mask covers bit 31 (GAME-H-6 fix)");
    ASSERT(mask == INT64_C(-1), "default mask covers all 64 bits");
    rt_obj_release_check0(body);
}

static void test_collision_layer31_collides_with_default_mask() {
    void *world = rt_physics2d_world_new(0.0, 0.0);

    // Body A on layer 31 — before fix it was excluded from the default mask
    void *a = rt_physics2d_body_new(0, 0, 20, 20, 1.0);
    rt_physics2d_body_set_collision_layer(a, (int64_t)1 << 31);
    // keep default mask (0xFFFFFFFF)

    // Body B on layer 1 with default mask
    void *b = rt_physics2d_body_new(10, 0, 20, 20, 1.0);

    rt_physics2d_body_set_vel(a, 5.0, 0.0);
    rt_physics2d_body_set_vel(b, -5.0, 0.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);

    double va_before = rt_physics2d_body_vx(a);
    rt_physics2d_world_step(world, 0.001);
    double va_after = rt_physics2d_body_vx(a);

    // Collision must have changed velocity (bodies overlap, masks allow collision)
    ASSERT(fabs(va_after - va_before) > EPSILON,
           "layer-31 body collides with default-mask body (GAME-H-6 fix)");

    rt_obj_release_check0(a);
    rt_obj_release_check0(b);
    rt_obj_release_check0(world);
}

static void test_collision_mask_filtering_works() {
    // Bodies on incompatible layers should NOT collide
    void *world = rt_physics2d_world_new(0.0, 0.0);

    void *a = rt_physics2d_body_new(0, 0, 20, 20, 1.0);
    rt_physics2d_body_set_collision_layer(a, 1);
    /// @brief Rt_physics2d_body_set_collision_mask.
    rt_physics2d_body_set_collision_mask(a, 2); // only collides with layer 2

    void *b = rt_physics2d_body_new(10, 0, 20, 20, 1.0);
    /// @brief Rt_physics2d_body_set_collision_layer.
    rt_physics2d_body_set_collision_layer(b, 4); // layer 4, not in A's mask
    rt_physics2d_body_set_collision_mask(b, 1);

    rt_physics2d_body_set_vel(a, 10.0, 0.0);
    rt_physics2d_body_set_vel(b, -10.0, 0.0);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);
    rt_physics2d_world_step(world, 0.001);

    // Velocities must be unchanged — no collision between incompatible layers
    ASSERT_NEAR(rt_physics2d_body_vx(a), 10.0, "layer-filtered A vx unchanged");
    ASSERT_NEAR(rt_physics2d_body_vx(b), -10.0, "layer-filtered B vx unchanged");

    rt_obj_release_check0(a);
    rt_obj_release_check0(b);
    rt_obj_release_check0(world);
}

static void test_clustered_broadphase_has_no_fixed_occupancy_limit() {
    void *world = rt_physics2d_world_new(0.0, 0.0);

    void *wall = rt_physics2d_body_new(0, 0, 20, 20, 0.0);
    rt_physics2d_body_set_collision_layer(wall, 1);
    rt_physics2d_body_set_collision_mask(wall, 1);
    rt_physics2d_world_add(world, wall);

    for (int i = 0; i < 32; i++) {
        void *filler = rt_physics2d_body_new(0, 0, 20, 20, 1.0);
        rt_physics2d_body_set_collision_layer(filler, 2);
        rt_physics2d_body_set_collision_mask(filler, 0);
        rt_physics2d_world_add(world, filler);
        release_obj(filler);
    }

    // This target is deliberately beyond the old grid's 32-body cell cap.
    void *target = rt_physics2d_body_new(1, 0, 20, 20, 1.0);
    rt_physics2d_body_set_collision_layer(target, 1);
    rt_physics2d_body_set_collision_mask(target, 1);
    rt_physics2d_body_set_vel(target, -5.0, 0.0);
    rt_physics2d_world_add(world, target);

    rt_physics2d_world_step(world, 0.001);
    ASSERT(fabs(rt_physics2d_body_vx(target) + 5.0) > EPSILON,
           "persistent sweep broad-phase resolves clustered bodies without a cell cap");
    auto *world_impl = static_cast<rt_world_impl *>(world);
    ASSERT(world_impl->broadphase_count == world_impl->body_count,
           "persistent broad-phase tracks every world body");

    release_obj(target);
    release_obj(wall);
    release_obj(world);
}

static void test_world_finalizer_releases_joint_retained_bodies() {
    g_body_finalizer_calls = 0;

    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    void *b = rt_physics2d_body_new(20, 0, 10, 10, 1.0);
    void *joint = rt_physics2d_distance_joint_new(a, b, 20.0);
    rt_obj_set_finalizer(a, test_body_finalizer);
    rt_obj_set_finalizer(b, test_body_finalizer);

    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);
    rt_physics2d_world_add_joint(world, joint);

    // Drop the caller's reference; the world should own the last remaining joint reference.
    ASSERT(rt_obj_release_check0(joint) == 0, "joint retained by world before world finalizer");

    release_obj(a);
    release_obj(b);
    release_obj(world);

    ASSERT(g_body_finalizer_calls == 2, "joint/world finalizers release retained bodies");
}

static void test_add_joint_requires_world_bodies() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    void *b = rt_physics2d_body_new(20, 0, 10, 10, 1.0);
    void *joint = rt_physics2d_distance_joint_new(a, b, 20.0);

    EXPECT_TRAP(rt_physics2d_world_add_joint(world, joint));

    release_obj(joint);
    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_joint_storage_grows_past_default_capacity() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0, 0, 10, 10, 1.0);
    void *b = rt_physics2d_body_new(20, 0, 10, 10, 1.0);
    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);

    const int total = PH_MAX_JOINTS + 5;
    for (int i = 0; i < total; i++) {
        void *joint = rt_physics2d_distance_joint_new(a, b, 20.0 + i);
        rt_physics2d_world_add_joint(world, joint);
        release_obj(joint);
    }
    ASSERT(rt_physics2d_world_joint_count(world) == total,
           "joint storage grows past PH_MAX_JOINTS");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

//=============================================================================
// GAME-C-4: PH_MAX_BODIES is an initial reservation, not a hard body cap
//=============================================================================

static void test_body_storage_grows_and_high_index_pair_resolves() {
    void *world = rt_physics2d_world_new(0.0, 0.0);

    void *wall = rt_physics2d_body_new(0, 0, 20, 20, 0.0);
    rt_physics2d_body_set_collision_layer(wall, 1);
    rt_physics2d_body_set_collision_mask(wall, 1);
    rt_physics2d_world_add(world, wall);

    for (int i = 0; i < PH_MAX_BODIES; i++) {
        void *filler = rt_physics2d_body_new((double)(i + 1) * 100.0, 1000.0, 1, 1, 1.0);
        rt_physics2d_body_set_collision_layer(filler, 2);
        rt_physics2d_body_set_collision_mask(filler, 0);
        rt_physics2d_world_add(world, filler);
        release_obj(filler);
    }

    void *target = rt_physics2d_body_new(1, 0, 20, 20, 1.0);
    rt_physics2d_body_set_collision_layer(target, 1);
    rt_physics2d_body_set_collision_mask(target, 1);
    rt_physics2d_body_set_vel(target, -5.0, 0.0);
    rt_physics2d_world_add(world, target);

    ASSERT(rt_physics2d_world_body_count(world) == PH_MAX_BODIES + 2,
           "body storage grows past PH_MAX_BODIES");

    rt_physics2d_world_step(world, 0.001);
    ASSERT(fabs(rt_physics2d_body_vx(target) + 5.0) > EPSILON,
           "high-index body pair resolves through dynamic pair scratch");

    release_obj(target);
    release_obj(wall);
    release_obj(world);
}

static void test_distance_joint_no_phantom_velocity() {
    // A bob hanging on a distance joint below a static anchor, under gravity. The
    // positional solver alone leaves the uncorrected radial velocity to accumulate
    // (vy grows ~ g*t forever). The velocity-projection pass must cancel it so the
    // bob settles instead of building unbounded "phantom" velocity.
    void *world = rt_physics2d_world_new(0.0, 10.0);
    void *anchor = rt_physics2d_body_new(0, 0, 2, 2, 0.0); // static
    void *bob = rt_physics2d_body_new(0, 20, 2, 2, 1.0);   // hangs 20 below
    rt_physics2d_world_add(world, anchor);
    rt_physics2d_world_add(world, bob);
    void *joint = rt_physics2d_distance_joint_new(anchor, bob, 20.0);
    rt_physics2d_world_add_joint(world, joint);

    for (int i = 0; i < 240; i++)
        rt_physics2d_world_step(world, 1.0 / 60.0);

    // Without the fix, vy ≈ g*t = 10*4 = 40 after 4s. With it, radial velocity is
    // cancelled each step so vy stays near zero.
    ASSERT(fabs(rt_physics2d_body_vy(bob)) < 5.0, "distance joint cancels phantom radial velocity");

    release_obj(joint);
    release_obj(anchor);
    release_obj(bob);
    release_obj(world);
}

static void test_aabb_containment_ejects_nearest_side() {
    // A small body sitting inside a large static box, near its right edge. The MTV
    // must eject it the short way (out the right side), not pick the wrong axis with
    // an inflated penetration depth (the old intersection-width bug).
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *big = rt_physics2d_body_new(0, 0, 100, 100, 0.0); // static, spans 0..100
    void *small = rt_physics2d_body_new(90, 45, 5, 5, 1.0); // inside, near right edge
    rt_physics2d_world_add(world, big);
    rt_physics2d_world_add(world, small);

    double x0 = rt_physics2d_body_x(small);
    rt_physics2d_world_step(world, 0.001);

    // Nearest exit is +x (10 units), not the y axis (50 units) the old formula chose.
    ASSERT(rt_physics2d_body_x(small) > x0 + 1.0,
           "contained body ejected toward nearest (right) side, not the wrong axis");
    ASSERT(rt_physics2d_world_contact_depth(world, 0) < 20.0,
           "containment penetration is the near-exit distance, not the far one");

    release_obj(big);
    release_obj(small);
    release_obj(world);
}

static void test_low_speed_contact_no_bounce() {
    // A body already overlapping a wall, approaching slowly (below the restitution
    // threshold). Restitution must be suppressed so it does not rebound — otherwise
    // resting bodies jitter forever.
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *wall = rt_physics2d_body_new(10, 0, 10, 10, 0.0); // static, 10..20
    void *ball = rt_physics2d_body_new(5, 0, 10, 10, 1.0);  // overlaps, 5..15
    rt_physics2d_body_set_restitution(ball, 1.0);
    rt_physics2d_body_set_restitution(wall, 1.0);
    rt_physics2d_body_set_vel(ball, 0.5, 0.0); // slow approach 0.5 < 1.0 threshold
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_add(world, ball);

    rt_physics2d_world_step(world, 0.001);

    // With the threshold, restitution is suppressed: the approach velocity is
    // removed but not reflected, so vx settles near 0 instead of bouncing to -0.5.
    ASSERT(rt_physics2d_body_vx(ball) > -0.1,
           "low-speed contact does not rebound (restitution threshold)");

    release_obj(wall);
    release_obj(ball);
    release_obj(world);
}

static void test_high_speed_contact_still_bounces() {
    // Regression guard for the restitution threshold: a fast approach must still
    // bounce (the threshold only suppresses low-speed rebound).
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *wall = rt_physics2d_body_new(10, 0, 10, 10, 0.0);
    void *ball = rt_physics2d_body_new(5, 0, 10, 10, 1.0);
    rt_physics2d_body_set_restitution(ball, 1.0);
    rt_physics2d_body_set_restitution(wall, 1.0);
    rt_physics2d_body_set_vel(ball, 50.0, 0.0); // fast approach, well above threshold
    rt_physics2d_world_add(world, wall);
    rt_physics2d_world_add(world, ball);

    rt_physics2d_world_step(world, 0.001);

    ASSERT(rt_physics2d_body_vx(ball) < 0.0, "high-speed contact still bounces back");

    release_obj(wall);
    release_obj(ball);
    release_obj(world);
}

//=============================================================================
// Audit regressions
//=============================================================================

static void test_body_cannot_belong_to_two_worlds() {
    void *world_a = rt_physics2d_world_new(0.0, 0.0);
    void *world_b = rt_physics2d_world_new(0.0, 0.0);
    void *body = rt_physics2d_body_new(0.0, 0.0, 1.0, 1.0, 1.0);

    rt_physics2d_world_add(world_a, body);
    EXPECT_TRAP(rt_physics2d_world_add(world_b, body));
    ASSERT(rt_physics2d_world_body_count(world_a) == 1,
           "failed cross-world add preserves source membership");
    ASSERT(rt_physics2d_world_body_count(world_b) == 0,
           "failed cross-world add leaves destination empty");

    rt_physics2d_world_remove(world_a, body);
    rt_physics2d_world_add(world_b, body);
    ASSERT(rt_physics2d_world_body_count(world_b) == 1,
           "removed body can be added to another world");

    release_obj(body);
    release_obj(world_a);
    release_obj(world_b);
}

static void test_circle_box_dimensions_remain_zero_after_step() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *circle = rt_physics2d_circle_body_new(0.0, 0.0, 0.25, 1.0);
    rt_physics2d_world_add(world, circle);
    rt_physics2d_world_step(world, 0.01);

    ASSERT(rt_physics2d_body_w(circle) == 0.0, "circle width remains zero after sanitize");
    ASSERT(rt_physics2d_body_h(circle) == 0.0, "circle height remains zero after sanitize");
    ASSERT_NEAR(rt_physics2d_body_radius(circle), 0.25, "circle radius survives a world step");

    release_obj(circle);
    release_obj(world);
}

static void test_tiny_mass_has_finite_consistent_inverse() {
    void *body =
        rt_physics2d_body_new(0.0, 0.0, 1.0, 1.0, std::numeric_limits<double>::denorm_min());
    auto *impl = static_cast<rt_body_impl *>(body);
    ASSERT(rt_physics2d_body_is_static(body) == 0, "tiny positive mass remains dynamic");
    ASSERT(std::isfinite(impl->inv_mass) && impl->inv_mass > 0.0,
           "tiny positive mass has finite inverse");

    impl->mass = 4.0;
    impl->inv_mass = 99.0;
    sanitize_body_state(impl);
    ASSERT_NEAR(impl->inv_mass, 0.25, "sanitize derives inverse mass from mass");

    release_obj(body);
}

static void test_invalid_contact_does_not_touch_capacity_or_overflow_count() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0.0, 0.0, 1.0, 1.0, 1.0);
    void *b = rt_physics2d_body_new(0.0, 0.0, 1.0, 1.0, 1.0);
    auto *impl = static_cast<rt_world_impl *>(world);

    impl->contact_count = INT64_MAX;
    world_record_contact(impl,
                         static_cast<rt_body_impl *>(a),
                         static_cast<rt_body_impl *>(b),
                         std::numeric_limits<double>::quiet_NaN(),
                         0.0,
                         0.0);
    ASSERT(impl->contact_count == INT64_MAX, "invalid manifold does not increment contact count");
    ASSERT(impl->contact_overflow == 0, "invalid manifold does not report storage overflow");
    impl->contact_count = 0;

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_undersized_same_class_handles_trap() {
    void *short_world =
        rt_obj_new_i64(RT_PHYSICS2D_WORLD_CLASS_ID, static_cast<int64_t>(sizeof(void *)));
    void *short_body =
        rt_obj_new_i64(RT_PHYSICS2D_BODY_CLASS_ID, static_cast<int64_t>(sizeof(void *)));
    void *short_joint =
        rt_obj_new_i64(RT_PHYSICS2D_JOINT_CLASS_ID, static_cast<int64_t>(sizeof(void *)));
    void *short_projectile =
        rt_obj_new_i64(RT_PHYSICS2D_PROJECTILE_CLASS_ID, static_cast<int64_t>(sizeof(void *)));

    EXPECT_TRAP(rt_physics2d_world_body_count(short_world));
    EXPECT_TRAP(rt_physics2d_body_x(short_body));
    EXPECT_TRAP(rt_physics2d_joint_get_type(short_joint));
    EXPECT_TRAP(rt_projectile2d_total_time(short_projectile));

    release_obj(short_world);
    release_obj(short_body);
    release_obj(short_joint);
    release_obj(short_projectile);
}

static void test_uninitialized_full_size_handles_trap() {
    void *forged_world =
        rt_obj_new_i64(RT_PHYSICS2D_WORLD_CLASS_ID, static_cast<int64_t>(sizeof(rt_world_impl)));
    void *forged_body =
        rt_obj_new_i64(RT_PHYSICS2D_BODY_CLASS_ID, static_cast<int64_t>(sizeof(rt_body_impl)));
    void *forged_joint =
        rt_obj_new_i64(RT_PHYSICS2D_JOINT_CLASS_ID, static_cast<int64_t>(sizeof(ph_joint)));
    void *forged_projectile = rt_obj_new_i64(RT_PHYSICS2D_PROJECTILE_CLASS_ID, 256);
    ASSERT(forged_world && forged_body && forged_joint && forged_projectile,
           "full-size forged objects allocate");

    memset(forged_world, 0, sizeof(rt_world_impl));
    memset(forged_body, 0, sizeof(rt_body_impl));
    memset(forged_joint, 0, sizeof(ph_joint));
    memset(forged_projectile, 0, 256);
    EXPECT_TRAP(rt_physics2d_world_body_count(forged_world));
    EXPECT_TRAP(rt_physics2d_body_x(forged_body));
    EXPECT_TRAP(rt_physics2d_joint_get_type(forged_joint));
    EXPECT_TRAP(rt_projectile2d_total_time(forged_projectile));

    release_obj(forged_world);
    release_obj(forged_body);
    release_obj(forged_joint);
    release_obj(forged_projectile);
}

static void test_subpicometer_aabb_sweep_is_not_discarded() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *mover = rt_physics2d_body_new(0.0, 0.0, 1.0e-15, 1.0e-15, 1.0);
    void *wall = rt_physics2d_body_new(2.0e-13, 0.0, 1.0e-15, 1.0e-15, 0.0);
    rt_physics2d_body_set_vel(mover, 1.0, 0.0);
    rt_physics2d_world_add(world, mover);
    rt_physics2d_world_add(world, wall);

    rt_physics2d_world_step(world, 5.0e-13);
    ASSERT(rt_physics2d_world_contact_count(world) == 1,
           "nonzero subpicometer sweep still reaches a thin wall");

    release_obj(mover);
    release_obj(wall);
    release_obj(world);
}

static void test_tiny_circle_overlap_keeps_geometric_normal() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_circle_body_new(0.0, 0.0, 1.0e-9, 0.0);
    void *b = rt_physics2d_circle_body_new(0.0, 1.0e-9, 1.0e-9, 0.0);
    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);

    rt_physics2d_world_step(world, 1.0e-6);
    ASSERT(rt_physics2d_world_contact_count(world) == 1, "tiny overlapping circles contact");
    ASSERT(std::fabs(rt_physics2d_world_contact_nx(world, 0)) < 1.0e-12,
           "tiny circle normal does not fall back to +X");
    ASSERT(rt_physics2d_world_contact_ny(world, 0) > 0.999999,
           "tiny circle normal follows center separation");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_huge_mass_collision_avoids_impulse_overflow() {
    void *world = rt_physics2d_world_new(0.0, 0.0);
    void *a = rt_physics2d_body_new(0.0, 0.0, 10.0, 10.0, DBL_MAX);
    void *b = rt_physics2d_body_new(9.0, 0.0, 10.0, 10.0, DBL_MAX);
    rt_physics2d_body_set_restitution(a, 1.0);
    rt_physics2d_body_set_restitution(b, 1.0);
    rt_physics2d_body_set_vel(a, 10.0, 0.0);
    rt_physics2d_body_set_vel(b, -10.0, 0.0);
    rt_physics2d_world_add(world, a);
    rt_physics2d_world_add(world, b);

    rt_physics2d_world_step(world, 0.001);
    ASSERT(rt_physics2d_body_vx(a) < -9.0, "huge-mass body A bounces without infinity");
    ASSERT(rt_physics2d_body_vx(b) > 9.0, "huge-mass body B bounces without infinity");

    release_obj(a);
    release_obj(b);
    release_obj(world);
}

static void test_projectile_numeric_edges() {
    void *small_drag = rt_projectile2d_new(0.0, 0.0, 10.0, 0.0, 0.0, 10.0);
    rt_projectile2d_set_drag(small_drag, 1.0e-20);
    ASSERT(std::fabs(rt_projectile2d_x_at(small_drag, 2.0) - 20.0) < 1.0e-9,
           "tiny drag position is continuous with ballistic motion");
    ASSERT(std::fabs(rt_projectile2d_y_at(small_drag, 2.0) - 20.0) < 1.0e-9,
           "tiny drag gravity term avoids cancellation");

    void *extreme = rt_projectile2d_new(DBL_MAX, 0.0, DBL_MAX, 0.0, DBL_MAX, 0.0);
    ASSERT(std::isfinite(rt_projectile2d_x_at(extreme, DBL_MAX)),
           "extreme projectile position saturates finite");
    ASSERT(std::isfinite(rt_projectile2d_vx_at(extreme, DBL_MAX)),
           "extreme projectile velocity saturates finite");

    void *already_grounded = rt_projectile2d_new(0.0, 5.0, 0.0, 0.0, 0.0, 10.0);
    rt_projectile2d_set_ground_y(already_grounded, 3.0);
    ASSERT(rt_projectile2d_time_to_ground(already_grounded) == 0.0,
           "projectile starting beyond ground reports zero impact time");

    void *tiny_gravity = rt_projectile2d_new(0.0, 0.0, 0.0, 0.0, 0.0, 1.0e-20);
    rt_projectile2d_set_ground_y(tiny_gravity, 1.0);
    double tiny_gravity_time = rt_projectile2d_time_to_ground(tiny_gravity);
    double expected_tiny_gravity_time = std::sqrt(2.0e20);
    ASSERT(std::isfinite(tiny_gravity_time) &&
               std::fabs(tiny_gravity_time / expected_tiny_gravity_time - 1.0) < 1.0e-12,
           "tiny nonzero gravity uses the quadratic path");

    void *cancellation = rt_projectile2d_new(0.0, 0.0, 0.0, 1.0e16, 0.0, 1.0);
    rt_projectile2d_set_ground_y(cancellation, 1.0);
    double cancellation_time = rt_projectile2d_time_to_ground(cancellation);
    ASSERT(cancellation_time > 0.0 && cancellation_time < 1.0e-15,
           "quadratic solver preserves a small positive root");

    void *arch = rt_projectile2d_new(0.0, 0.0, 0.0, 10.0, 0.0, -100.0);
    rt_projectile2d_set_drag(arch, 0.5);
    rt_projectile2d_set_ground_y(arch, 0.2);
    double arch_impact = rt_projectile2d_time_to_ground(arch);
    ASSERT(std::isfinite(arch_impact) && arch_impact > 0.0 && arch_impact < 0.1,
           "dragged upward arch finds an early crossing before returning");
    rt_projectile2d_advance(arch, 1.0);
    ASSERT(rt_projectile2d_has_landed(arch) == 1,
           "advance latches a ground crossing even when the endpoint returned above ground");

    void *asymptote = rt_projectile2d_new(0.0, 0.0, 0.0, 10.0, 0.0, 0.0);
    rt_projectile2d_set_drag(asymptote, 2.0);
    rt_projectile2d_set_ground_y(asymptote, 5.0);
    ASSERT(std::isinf(rt_projectile2d_time_to_ground(asymptote)),
           "drag asymptote equal to ground is not a finite crossing");

    release_obj(small_drag);
    release_obj(extreme);
    release_obj(already_grounded);
    release_obj(tiny_gravity);
    release_obj(cancellation);
    release_obj(arch);
    release_obj(asymptote);
}

/// @brief Main.
int main() {
    // World tests
    test_world_new();
    test_world_add_remove();
    test_world_add_multiple();
    test_duplicate_body_add_ignored();
    test_body_cannot_belong_to_two_worlds();

    // Body tests
    test_body_new();
    test_body_static();
    test_body_set_pos();
    test_body_set_vel();
    test_body_restitution_friction();
    test_body_inputs_sanitized();
    test_static_set_vel_ignored();
    test_restitution_friction_clamped();
    test_circle_box_dimensions_remain_zero_after_step();
    test_tiny_mass_has_finite_consistent_inverse();

    // Integration tests
    test_gravity_integration();
    test_velocity_integration();
    test_static_body_no_gravity();
    test_force_application();
    test_impulse_application();

    // Collision tests
    test_collision_detection();
    test_no_collision_separated();
    test_collision_with_static();
    test_set_gravity();
    test_invalid_dt_noop();
    test_large_dt_is_deferred_instead_of_discarded();
    test_zero_gravity_body_sleeps_and_external_force_wakes_it();
    test_separating_overlap_still_corrected();
    test_circle_body_collides_with_aabb();
    test_swept_aabb_hits_thin_wall();
    test_swept_circle_hits_thin_wall();
    test_swept_circle_misses_near_diagonal_target();
    test_circle_inside_aabb_resolves_outward();
    test_circle_radius_allows_subunit_values();
    test_circle_aabb_tangent_is_not_overlap();
    test_world_records_step_contacts();
    test_world_deduplicates_contacts_across_substeps();
    test_world_clears_contacts_on_noop_step();
    test_world_remove_clears_contacts();
    test_world_contact_storage_grows_past_default_capacity();
    test_extreme_forces_are_sanitized();
    test_invalid_contact_does_not_touch_capacity_or_overflow_count();
    test_subpicometer_aabb_sweep_is_not_discarded();
    test_tiny_circle_overlap_keeps_geometric_normal();
    test_huge_mass_collision_avoids_impulse_overflow();

    // Safety tests
    test_null_safety();
    test_wrong_handle_traps();
    test_undersized_same_class_handles_trap();
    test_uninitialized_full_size_handles_trap();
    test_zero_dt();
    test_projectile_numeric_edges();

    // GAME-H-6: collision_mask default covers all 32 layers
    test_collision_mask_default_full();
    test_collision_layer31_collides_with_default_mask();
    test_collision_mask_filtering_works();
    test_clustered_broadphase_has_no_fixed_occupancy_limit();
    test_world_finalizer_releases_joint_retained_bodies();
    test_add_joint_requires_world_bodies();
    test_joint_storage_grows_past_default_capacity();

    // GAME-C-4: PH_MAX_BODIES is an initial reservation
    test_body_storage_grows_and_high_index_pair_resolves();

    // Solver-correctness fixes
    test_distance_joint_no_phantom_velocity();
    test_aabb_containment_ejects_nearest_side();
    test_low_speed_contact_no_bounce();
    test_high_speed_contact_still_bounces();

    printf("Physics2D tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
