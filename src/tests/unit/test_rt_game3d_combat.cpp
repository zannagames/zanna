//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_combat.cpp
// Purpose: Unit tests for the Game3D combat layer — Hitbox3D hit/hurt volume
//   overlap with team/channel/rehit filtering and animation-window activation,
//   the polled HitEvent3D buffer, and the Health3D damage/i-frame/death
//   lifecycle with the polled DamageEvent3D buffer and knockback helper.
// Key invariants:
//   - Deterministic: fixed steps only; replays produce identical event counts.
// Ownership/Lifetime:
//   - Test-created runtime handles rely on production GC conventions.
// Links: src/runtime/graphics/3d/rt_game3d_combat.c
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_animcontroller3d.h"
#include "rt_collider3d.h"
#include "rt_game3d.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_skeleton3d.h"
#include "rt_string.h"
#include "rt_vec3.h"

#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
static std::jmp_buf g_trap_jmp;
static const char *g_last_trap = nullptr;
static bool g_expect_trap = false;
static int g_tests_passed = 0;
static int g_tests_total = 0;
} // namespace

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_expect_trap)
        std::longjmp(g_trap_jmp, 1);
    std::fprintf(stderr, "unexpected runtime trap: %s\n", msg ? msg : "(null)");
    std::abort();
}

#define TEST(name)                                                                                 \
    do {                                                                                           \
        ++g_tests_total;                                                                           \
        std::printf("  [%d] %s... ", g_tests_total, name);                                         \
    } while (0)

#define PASS()                                                                                     \
    do {                                                                                           \
        ++g_tests_passed;                                                                          \
        std::printf("ok\n");                                                                       \
        return true;                                                                               \
    } while (0)

#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        std::printf("FAIL: %s\n", msg);                                                            \
        return false;                                                                              \
    } while (0)

#define EXPECT_TRUE(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond))                                                                               \
            FAIL(msg);                                                                             \
    } while (0)

#define EXPECT_EQ_INT(actual, expected, msg)                                                       \
    do {                                                                                           \
        const long long got_ = (long long)(actual);                                                \
        const long long want_ = (long long)(expected);                                             \
        if (got_ != want_) {                                                                       \
            std::printf("FAIL: %s (got %lld, expected %lld)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(actual, expected, eps, msg)                                                    \
    do {                                                                                           \
        const double got_ = (double)(actual);                                                      \
        const double want_ = (double)(expected);                                                   \
        if (std::fabs(got_ - want_) > (eps)) {                                                     \
            std::printf("FAIL: %s (got %.6f, expected %.6f)\n", msg, got_, want_);                 \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

/// @brief Fighter fixture: two entities one unit apart, attacker hit-sphere
///   reaching the victim's hurt-sphere, on different teams by default.
struct CombatFixture {
    void *world = nullptr;
    void *attacker = nullptr;
    void *victim = nullptr;
    void *hit = nullptr;
    void *hurt = nullptr;
};

CombatFixture combat_fixture_new(const char *title) {
    CombatFixture fx;
    fx.world = rt_game3d_world_new(rt_const_cstr(title), 64, 48);
    rt_game3d_world_set_gravity(fx.world, 0.0, 0.0, 0.0);
    fx.attacker = rt_game3d_entity_new();
    rt_game3d_entity_set_position(fx.attacker, 0.0, 1.0, 0.0);
    rt_game3d_world_spawn(fx.world, fx.attacker);
    fx.victim = rt_game3d_entity_new();
    rt_game3d_entity_set_position(fx.victim, 0.0, 1.0, -1.0);
    rt_game3d_world_spawn(fx.world, fx.victim);

    void *hit_shape = rt_collider3d_new_sphere(0.4);
    fx.hit = rt_game3d_hitbox_new(fx.attacker, hit_shape);
    if (rt_obj_release_check0(hit_shape))
        rt_obj_free(hit_shape);
    rt_game3d_hitbox_set_kind(fx.hit, 1);
    rt_game3d_hitbox_set_team(fx.hit, 1);
    rt_game3d_hitbox_set_local_offset(fx.hit, 0.0, 0.0, -0.7);

    void *hurt_shape = rt_collider3d_new_sphere(0.5);
    fx.hurt = rt_game3d_hitbox_new(fx.victim, hurt_shape);
    if (rt_obj_release_check0(hurt_shape))
        rt_obj_free(hurt_shape);
    rt_game3d_hitbox_set_team(fx.hurt, 2);
    return fx;
}

bool test_combat_overlap_hit_and_rehit() {
    TEST("Combat pass emits one hit per activation with rehit suppression");
    CombatFixture fx = combat_fixture_new("Combat Overlap");
    rt_game3d_hitbox_set_active(fx.hit, 1);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx.world), 1, "first step emits one hit");
    void *event = rt_game3d_world_hit_event(fx.world, 0);
    EXPECT_TRUE(event != nullptr, "hit event wraps");
    EXPECT_TRUE(rt_game3d_hit_event_get_attacker(event) == fx.attacker, "attacker matches");
    EXPECT_TRUE(rt_game3d_hit_event_get_victim(event) == fx.victim, "victim matches");
    EXPECT_TRUE(rt_game3d_hit_event_get_hitbox(event) == fx.hit, "hitbox matches");
    EXPECT_TRUE(rt_game3d_hit_event_get_hurtbox(event) == fx.hurt, "hurtbox matches");
    void *normal = rt_game3d_hit_event_normal(event);
    EXPECT_TRUE(normal != nullptr, "normal is a Vec3");
    if (rt_obj_release_check0(normal))
        rt_obj_free(normal);

    /* Still overlapped: no re-hit while the activation persists. */
    for (int i = 0; i < 10; ++i) {
        rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
        EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx.world), 0, "no rehit while active");
    }

    /* Deactivate + reactivate: a fresh swing hits again. */
    rt_game3d_hitbox_set_active(fx.hit, 0);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    rt_game3d_hitbox_set_active(fx.hit, 1);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx.world), 1, "new activation hits again");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_combat_filters() {
    TEST("Combat filters: team, friendly fire, channels, self");
    /* Same team: no event. */
    CombatFixture fx = combat_fixture_new("Combat Team");
    rt_game3d_hitbox_set_team(fx.hurt, 1);
    rt_game3d_hitbox_set_active(fx.hit, 1);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx.world), 0, "same team is skipped");
    /* Friendly fire admits the same-team pair (fresh activation). */
    rt_game3d_hitbox_set_active(fx.hit, 0);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    rt_game3d_hitbox_set_friendly_fire(fx.hit, 1);
    rt_game3d_hitbox_set_active(fx.hit, 1);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx.world), 1, "friendly fire admits");
    rt_game3d_world_destroy(fx.world);

    /* Disjoint channels: no event. */
    CombatFixture fx2 = combat_fixture_new("Combat Channel");
    rt_game3d_hitbox_set_channel(fx2.hit, 2);
    rt_game3d_hitbox_set_channel(fx2.hurt, 4);
    rt_game3d_hitbox_set_active(fx2.hit, 1);
    rt_game3d_world_step_simulation(fx2.world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx2.world), 0, "disjoint channels skip");
    rt_game3d_world_destroy(fx2.world);

    /* Self pair: attacker's own hurt volume is never hit by its own swing. */
    CombatFixture fx3 = combat_fixture_new("Combat Self");
    void *self_shape = rt_collider3d_new_sphere(0.5);
    void *self_hurt = rt_game3d_hitbox_new(fx3.attacker, self_shape);
    if (rt_obj_release_check0(self_shape))
        rt_obj_free(self_shape);
    rt_game3d_hitbox_set_team(self_hurt, 2); /* different team, same entity */
    rt_game3d_hitbox_set_active(fx3.hit, 1);
    rt_game3d_world_step_simulation(fx3.world, 1.0 / 60.0);
    int64_t count = rt_game3d_world_hit_event_count(fx3.world);
    for (int64_t i = 0; i < count; ++i) {
        void *event = rt_game3d_world_hit_event(fx3.world, i);
        EXPECT_TRUE(rt_game3d_hit_event_get_victim(event) != fx3.attacker,
                    "self pairs are never reported");
    }
    rt_game3d_world_destroy(fx3.world);
    PASS();
}

bool test_combat_window_activation() {
    TEST("Animation windows gate hitbox liveness by state and time");
    CombatFixture fx = combat_fixture_new("Combat Window");
    /* Scripted animator: one 1-second 'Swing' state on a bare skeleton. */
    void *skeleton = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skeleton, rt_const_cstr("root"), -1, nullptr);
    void *controller = rt_anim_controller3d_new(skeleton);
    void *clip = rt_animation3d_new(rt_const_cstr("SwingClip"), 1.0);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("Swing"), clip);
    rt_game3d_entity_attach_animator(fx.attacker, controller);
    rt_anim_controller3d_play(controller, rt_const_cstr("Swing"));

    /* Window opens at 0.25 s. Manual switch stays off. */
    rt_game3d_hitbox_bind_window(fx.hit, rt_const_cstr("Swing"), 0.25, 0.6);
    int64_t hits_before_window = 0;
    int64_t hits_total = 0;
    for (int i = 0; i < 30; ++i) { /* 30 × (1/60) = 0.5 s of state time */
        rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
        int64_t count = rt_game3d_world_hit_event_count(fx.world);
        hits_total += count;
        if (i < 12) /* state time still below ~0.2 s */
            hits_before_window += count;
    }
    EXPECT_EQ_INT(hits_before_window, 0, "no hits before the window opens");
    EXPECT_EQ_INT(hits_total, 1, "exactly one hit per swing window");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_combat_window_survives_coarse_step() {
    TEST("Narrow hitbox windows cannot be jumped over by a coarse step");
    CombatFixture fx = combat_fixture_new("Combat WindowCoarse");
    void *skeleton = rt_skeleton3d_new();
    rt_skeleton3d_add_bone(skeleton, rt_const_cstr("root"), -1, nullptr);
    void *controller = rt_anim_controller3d_new(skeleton);
    void *clip = rt_animation3d_new(rt_const_cstr("SwingClip"), 1.0);
    rt_anim_controller3d_add_state(controller, rt_const_cstr("Swing"), clip);
    rt_game3d_entity_attach_animator(fx.attacker, controller);
    rt_anim_controller3d_play(controller, rt_const_cstr("Swing"));

    /* A 50 ms window sampled at dt = 0.2 s never lands INSIDE [0.25, 0.30]
     * (state times 0.2, 0.4, 0.6, 0.8); only [prev, now] crossing detection
     * can fire it. */
    rt_game3d_hitbox_bind_window(fx.hit, rt_const_cstr("Swing"), 0.25, 0.30);
    int64_t hits_total = 0;
    for (int i = 0; i < 4; ++i) {
        rt_game3d_world_step_simulation(fx.world, 0.2);
        hits_total += rt_game3d_world_hit_event_count(fx.world);
    }
    EXPECT_EQ_INT(hits_total, 1, "coarse step crosses the narrow window exactly once");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_health_lifecycle_and_events() {
    TEST("Health3D damage/death lifecycle with damage events");
    CombatFixture fx = combat_fixture_new("Combat Health");
    void *health = rt_game3d_health_new(100.0);
    rt_game3d_entity_attach_health(fx.victim, health);
    EXPECT_TRUE(rt_game3d_entity_get_health(fx.victim) == health, "health slot resolves");

    EXPECT_NEAR(rt_game3d_health_damage(health, 40.0, fx.attacker, 7),
                40.0,
                1e-9,
                "first damage applies fully");
    EXPECT_NEAR(rt_game3d_health_get_current(health), 60.0, 1e-9, "hp drops to 60");
    EXPECT_TRUE(rt_game3d_health_just_damaged(health) != 0, "just_damaged latches");
    EXPECT_EQ_INT(rt_game3d_world_damage_event_count(fx.world), 1, "damage event buffered");
    void *event = rt_game3d_world_damage_event(fx.world, 0);
    EXPECT_TRUE(rt_game3d_damage_event_get_victim(event) == fx.victim, "event victim");
    EXPECT_TRUE(rt_game3d_damage_event_get_source(event) == fx.attacker, "event source");
    EXPECT_NEAR(rt_game3d_damage_event_get_amount(event), 40.0, 1e-9, "event amount");
    EXPECT_EQ_INT(rt_game3d_damage_event_get_tag(event), 7, "event tag");
    EXPECT_TRUE(rt_game3d_damage_event_get_was_lethal(event) == 0, "not lethal yet");

    /* Lethal blow (i-frames disabled for this test). */
    EXPECT_NEAR(rt_game3d_health_damage(health, 70.0, nullptr, 8),
                60.0,
                1e-9,
                "lethal damage clamps to remaining hp");
    EXPECT_TRUE(rt_game3d_health_is_dead(health) != 0, "death latches");
    EXPECT_TRUE(rt_game3d_health_just_died(health) != 0, "just_died latches");
    EXPECT_NEAR(rt_game3d_health_damage(health, 10.0, nullptr, 9),
                0.0,
                1e-9,
                "damage while dead applies nothing");
    EXPECT_EQ_INT(rt_game3d_world_damage_event_count(fx.world), 2, "two events buffered");
    void *lethal = rt_game3d_world_damage_event(fx.world, 1);
    EXPECT_TRUE(rt_game3d_damage_event_get_was_lethal(lethal) != 0, "second event lethal");
    EXPECT_TRUE(rt_game3d_damage_event_get_source(lethal) == nullptr, "null source allowed");

    /* One-shot flags clear on the next step; revive restores. */
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_TRUE(rt_game3d_health_just_damaged(health) == 0, "just_damaged clears next step");
    EXPECT_TRUE(rt_game3d_health_just_died(health) == 0, "just_died clears next step");
    rt_game3d_health_revive(health, 50.0);
    EXPECT_TRUE(rt_game3d_health_is_dead(health) == 0, "revive clears death");
    EXPECT_NEAR(rt_game3d_health_get_current(health), 50.0, 1e-9, "revive hp applied");
    rt_game3d_health_heal(health, 100.0);
    EXPECT_NEAR(rt_game3d_health_get_current(health), 100.0, 1e-9, "heal clamps to max");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_health_iframes() {
    TEST("Health3D i-frames block repeat damage until they expire");
    CombatFixture fx = combat_fixture_new("Combat IFrames");
    void *health = rt_game3d_health_new(100.0);
    rt_game3d_health_set_invuln_seconds(health, 0.5);
    rt_game3d_entity_attach_health(fx.victim, health);

    EXPECT_NEAR(rt_game3d_health_damage(health, 10.0, nullptr, 0), 10.0, 1e-9, "first hit");
    EXPECT_TRUE(rt_game3d_health_get_invulnerable(health) != 0, "i-frames granted");
    /* 0.1 s later: still invulnerable. */
    for (int i = 0; i < 6; ++i)
        rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_NEAR(rt_game3d_health_damage(health, 10.0, nullptr, 0),
                0.0,
                1e-9,
                "damage inside the grace window is blocked");
    /* 0.6 s total: expired. */
    for (int i = 0; i < 30; ++i)
        rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_NEAR(rt_game3d_health_damage(health, 10.0, nullptr, 0),
                10.0,
                1e-9,
                "damage applies after i-frames expire");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_health_knockback() {
    TEST("Health3D knockback impulses dynamic bodies and rejects kinematic ones");
    CombatFixture fx = combat_fixture_new("Combat Knockback");
    /* Dynamic body on the victim. */
    void *body = rt_body3d_new_aabb(0.4, 0.4, 0.4, 2.0);
    rt_game3d_entity_attach_body(fx.victim, body);
    void *health = rt_game3d_health_new(100.0);
    rt_game3d_entity_attach_health(fx.victim, health);
    void *direction = rt_vec3_new(0.0, 0.0, -1.0);
    void *point = rt_vec3_new(0.0, 1.0, -1.0);
    EXPECT_TRUE(rt_game3d_health_apply_knockback(health, direction, 8.0, point) != 0,
                "dynamic body accepts knockback");
    void *velocity = rt_body3d_get_velocity(body);
    EXPECT_NEAR(rt_vec3_z(velocity), -4.0, 0.01, "velocity change = impulse / mass");
    if (rt_obj_release_check0(velocity))
        rt_obj_free(velocity);

    /* Kinematic body: helper reports false. */
    rt_body3d_set_kinematic(body, 1);
    EXPECT_TRUE(rt_game3d_health_apply_knockback(health, direction, 8.0, point) == 0,
                "kinematic body rejects knockback");
    if (rt_obj_release_check0(point))
        rt_obj_free(point);
    if (rt_obj_release_check0(direction))
        rt_obj_free(direction);
    rt_game3d_world_destroy(fx.world);
    PASS();
}

bool test_combat_stale_entity_safety() {
    TEST("Despawned victims stop producing events; teardown fails closed");
    CombatFixture fx = combat_fixture_new("Combat Stale");
    rt_game3d_hitbox_set_active(fx.hit, 1);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_world_hit_event_count(fx.world), 1, "baseline hit");
    rt_game3d_world_despawn(fx.world, fx.victim);
    rt_game3d_hitbox_set_active(fx.hit, 0);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    rt_game3d_hitbox_set_active(fx.hit, 1);
    rt_game3d_world_step_simulation(fx.world, 1.0 / 60.0);
    EXPECT_EQ_INT(
        rt_game3d_world_hit_event_count(fx.world), 0, "despawned victim produces no events");
    rt_game3d_world_destroy(fx.world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D combat layer tests\n");
    bool ok = true;
    ok = test_combat_overlap_hit_and_rehit() && ok;
    ok = test_combat_filters() && ok;
    ok = test_combat_window_activation() && ok;
    ok = test_combat_window_survives_coarse_step() && ok;
    ok = test_health_lifecycle_and_events() && ok;
    ok = test_health_iframes() && ok;
    ok = test_health_knockback() && ok;
    ok = test_combat_stale_entity_safety() && ok;
    std::printf("\nCombat layer tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
