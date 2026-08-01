//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_rt_game3d_runtime_audit.cpp
// Purpose: Regression tests for the July 2026 Game3D gameplay-runtime audit:
//   bounded private state, stale entities, allocation-free line-of-sight,
//   behavior-tree topology, interaction focus, minimap ids, and footstep tables.
// Key invariants:
//   - Corrupt fixed-array counts never produce out-of-bounds access.
//   - Entity-owned components reject destroyed owners and stale targets.
//   - Graph and identifier arithmetic remains bounded at integer limits.
// Ownership/Lifetime:
//   - Creation references are released when practical; World3D teardown owns
//     and releases its spawned entity/component graph.
// Links: docs/internals/graphics3d-game-runtime-audit-2026-07.md, rt_game3d.h
//
//===----------------------------------------------------------------------===//

#ifndef ZANNA_ENABLE_GRAPHICS
#define ZANNA_ENABLE_GRAPHICS 1
#endif

#include "rt_game3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_string.h"
#include "rt_vec3.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <cstdint>
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

namespace {

template <typename Fn> bool expect_trap_contains(Fn &&fn, const char *needle) {
    g_last_trap = nullptr;
    g_expect_trap = true;
    if (setjmp(g_trap_jmp) == 0) {
        fn();
        g_expect_trap = false;
        return false;
    }
    g_expect_trap = false;
    return g_last_trap && (!needle || std::strstr(g_last_trap, needle));
}

void release_creation_ref(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

void *spawn_box(void *world, double x, double y, double z, double half) {
    void *entity = rt_game3d_entity_new();
    void *body = rt_body3d_new_aabb(half, half, half, 0.0);
    rt_game3d_entity_attach_body(entity, body);
    rt_game3d_entity_set_position(entity, x, y, z);
    rt_game3d_world_spawn(world, entity);
    return entity;
}

struct AuditPerceptTrack {
    void *entity;
    int64_t entity_id;
    double visible_time;
    double lost_time;
    double last_known[3];
    int8_t seen;
    int8_t touched;
};

struct AuditHeardEvent {
    double position[3];
    double loudness;
    int64_t tag;
};

struct AuditPerception {
    void *vptr;
    void *entity;
    double sight_range;
    double fov_degrees;
    double eye_height;
    double hearing_range;
    int64_t target_mask;
    int64_t los_mask;
    double time_to_see;
    double time_to_lose;
    AuditPerceptTrack tracks[16];
    int32_t track_count;
    AuditHeardEvent heard[8];
    int32_t heard_count;
    int8_t seen_changed;
};

struct AuditWorldPrefix {
    void *canvas;
    void *camera;
    void *scene;
    void *physics;
    void *input;
    void *audio;
    void *effects;
    void *stream;
    void *camera_controller;
    void **entities;
    int32_t entity_count;
    int32_t entity_capacity;
};

bool test_perception_self_occlusion_counts_and_hearing_overflow() {
    TEST("Perception3D ignores its owner body and bounds counts/reach arithmetic");
    void *world = rt_game3d_world_new(rt_const_cstr("Perception audit"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *owner = spawn_box(world, 0.0, 0.0, 0.0, 0.75);
    void *target = spawn_box(world, 0.0, 0.0, -3.0, 0.25);
    void *sense = rt_game3d_perception_new(owner);
    EXPECT_TRUE(sense != nullptr, "live owner accepts a perception component");
    rt_game3d_perception_set_sight(sense, 10.0, 180.0, 0.0);
    for (int i = 0; i < 24; ++i)
        rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_perception_seen_count(sense),
                  1,
                  "owner collider does not occlude the target sight ray");

    auto *sense_view = static_cast<AuditPerception *>(sense);
    sense_view->track_count = INT32_MAX;
    EXPECT_EQ_INT(rt_game3d_perception_seen_count(sense),
                  1,
                  "corrupt track count is clamped to fixed storage");
    sense_view->track_count = 1;
    sense_view->tracks[0].entity_id++;
    EXPECT_TRUE(rt_game3d_perception_seen_target(sense, 0) == nullptr,
                "visible-list query rejects a reused-address id mismatch");
    void *unknown = rt_game3d_perception_last_known_position(sense, target);
    EXPECT_TRUE(rt_vec3_z(unknown) == 0.0,
                "last-known query also requires the track's stable entity id");
    release_creation_ref(unknown);
    sense_view->tracks[0].entity_id--;
    rt_weak_store(&sense_view->tracks[0].entity, target);
    EXPECT_EQ_INT(rt_game3d_perception_seen_count(sense),
                  1,
                  "restored live weak track remains visible before despawn");
    release_creation_ref(target);
    rt_game3d_world_despawn(world, target);
    EXPECT_EQ_INT(rt_game3d_perception_seen_count(sense),
                  0,
                  "despawning the last strong target reference removes it from the seen count");
    EXPECT_TRUE(rt_game3d_perception_seen_target(sense, 0) == nullptr,
                "despawning the last strong target reference zeroes the perception track");

    rt_game3d_perception_set_hearing(sense, 1.0);
    void *sound_pos = rt_vec3_new(100.0, 0.0, 0.0);
    auto *world_view = static_cast<AuditWorldPrefix *>(world);
    double ray_origin[3] = {0.0, 0.0, 0.0};
    double ray_direction[3] = {0.0, 0.0, -1.0};
    void *ray_bodies[1] = {nullptr};
    EXPECT_EQ_INT(rt_world3d_raycast_all_bodies_raw(
                      world_view->physics, ray_origin, ray_direction, -1.0, -1, ray_bodies, 1),
                  -1,
                  "raw ray failure stays distinct from a valid empty hit set");
    int32_t saved_entity_count = world_view->entity_count;
    world_view->entity_count = -1;
    rt_game3d_world_report_sound(world, sound_pos, 1.0, 17);
    EXPECT_EQ_INT(rt_game3d_perception_heard_count(sense),
                  0,
                  "negative world count fails closed instead of scanning capacity");
    world_view->entity_count = saved_entity_count;
    rt_game3d_world_report_sound(world, sound_pos, DBL_MAX, 23);
    EXPECT_EQ_INT(rt_game3d_perception_heard_count(sense),
                  1,
                  "overflowed reach is treated as effectively unbounded");
    EXPECT_EQ_INT(rt_game3d_perception_heard_tag(sense, 0), 23, "heard event keeps its tag");
    sense_view->heard_count = INT32_MAX;
    EXPECT_EQ_INT(rt_game3d_perception_heard_count(sense),
                  8,
                  "corrupt heard count is clamped to the event buffer");
    sense_view->heard_count = 1;

    release_creation_ref(sound_pos);
    rt_game3d_world_destroy(world);
    PASS();
}

struct AuditBtInstance {
    void *vptr;
    void *entity;
    void *tree;
    void *target;
    double timers[128];
    int32_t running_child[128];
    int8_t custom_pending[128];
    int8_t custom_result[128];
    int64_t pending_custom_id;
    int32_t pending_custom_node;
};

bool test_behavior_tree_topology_and_pending_bounds() {
    TEST("BehaviorTree3D rejects malformed topology and bounds pending resolution");
    void *tree = rt_game3d_btree_new();
    int64_t outer = rt_game3d_btree_sequence(tree);
    int64_t inner = rt_game3d_btree_sequence(tree);
    int64_t inverter = rt_game3d_btree_inverter(tree);
    int64_t wait = rt_game3d_btree_wait(tree, 0.0);
    int64_t custom = rt_game3d_btree_custom(tree, 99);
    rt_game3d_btree_add_child(tree, outer, inner);
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_btree_add_child(tree, outer, outer); }, "cycle"),
        "direct self-cycle is rejected with a diagnostic");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_btree_add_child(tree, inner, outer); }, "cycle"),
        "indirect cycle is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_btree_add_child(tree, wait, custom); }, "leaf"),
        "leaf child edge is rejected");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_btree_add_child(tree, outer, inner); }, "duplicate"),
        "duplicate child edge is rejected");
    rt_game3d_btree_add_child(tree, inverter, wait);
    EXPECT_TRUE(expect_trap_contains([&] { rt_game3d_btree_add_child(tree, inverter, custom); },
                                     "exactly one"),
                "inverter rejects a second child");
    EXPECT_TRUE(expect_trap_contains([&] { (void)rt_game3d_btree_custom(tree, 0); }, "reserved"),
                "custom id zero is rejected");

    rt_game3d_btree_add_child(tree, inner, custom);
    rt_game3d_btree_set_root(tree, outer);
    void *world = rt_game3d_world_new(rt_const_cstr("BT audit"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *owner = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, owner);
    void *instance = rt_game3d_bt_instance_new(owner, tree);
    EXPECT_TRUE(instance != nullptr, "rooted acyclic tree creates an instance");
    EXPECT_TRUE(
        expect_trap_contains([&] { rt_game3d_bt_instance_set_target(instance, tree); }, "Entity3D"),
        "wrong-class target is rejected without silently clearing state");
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_EQ_INT(
        rt_game3d_bt_instance_pending_custom(instance), 99, "custom leaf becomes pending");

    auto *instance_view = static_cast<AuditBtInstance *>(instance);
    instance_view->pending_custom_id = 77;
    instance_view->pending_custom_node = INT32_MAX;
    rt_game3d_bt_instance_resolve(instance, 1);
    EXPECT_EQ_INT(rt_game3d_bt_instance_pending_custom(instance),
                  0,
                  "corrupt pending node is cleared without indexing the result arrays");
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_bt_instance_pending_custom(instance),
                  99,
                  "invalid pending state clears orphan per-node flags and can recover");
    rt_game3d_bt_instance_resolve(instance, 1);
    instance_view->pending_custom_id = 77;
    instance_view->pending_custom_node = -2;
    EXPECT_EQ_INT(rt_game3d_bt_instance_pending_custom(instance),
                  0,
                  "pending getter repairs a mismatched negative node and nonzero id");

    rt_game3d_world_destroy(world);
    release_creation_ref(tree);
    PASS();
}

bool test_behavior_tree_target_weak_lifetime() {
    TEST("BehaviorTreeInstance3D zeroes a target freed by despawn");
    void *tree = rt_game3d_btree_new();
    int64_t root = rt_game3d_btree_selector(tree);
    int64_t move = rt_game3d_btree_move_to_target(tree, 2.0, 0.25);
    int64_t fallback = rt_game3d_btree_custom(tree, 701);
    rt_game3d_btree_add_child(tree, root, move);
    rt_game3d_btree_add_child(tree, root, fallback);
    rt_game3d_btree_set_root(tree, root);

    void *world = rt_game3d_world_new(rt_const_cstr("BT weak-target audit"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *owner = rt_game3d_entity_new();
    void *target = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, owner);
    rt_game3d_world_spawn(world, target);
    void *instance = rt_game3d_bt_instance_new(owner, tree);
    rt_game3d_bt_instance_set_target(instance, target);

    release_creation_ref(instance);
    release_creation_ref(owner);
    release_creation_ref(target);
    rt_game3d_world_despawn(world, target);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    EXPECT_EQ_INT(rt_game3d_bt_instance_pending_custom(instance),
                  701,
                  "destroyed weak target fails movement and selects the fallback leaf");

    rt_game3d_world_destroy(world);
    release_creation_ref(world);
    release_creation_ref(tree);
    PASS();
}

struct AuditInteractor {
    void *vptr;
    void *entity;
    double cone_degrees;
    int64_t los_mask;
    int8_t require_los;
    void *focused;
    int8_t focus_changed;
    int8_t interact_requested;
    int64_t interact_count;
    void *last_interacted;
};

bool test_interaction_los_stale_focus_and_saturation() {
    TEST("Interactor3D ignores its owner body and revalidates focused targets");
    void *world = rt_game3d_world_new(rt_const_cstr("Interaction audit"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *owner = spawn_box(world, 0.0, 0.0, 0.0, 0.6);
    void *candidate = spawn_box(world, 0.0, 0.0, -1.5, 0.25);
    void *item = rt_game3d_interactable_new(candidate);
    void *scanner = rt_game3d_interactor_new(owner);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *focused = rt_game3d_interactor_get_focused(scanner);
    EXPECT_TRUE(focused == item, "owner collider does not block interaction line of sight");
    release_creation_ref(focused);

    auto *scanner_view = static_cast<AuditInteractor *>(scanner);
    scanner_view->interact_count = INT64_MAX;
    EXPECT_TRUE(rt_game3d_interactor_interact(scanner) != 0, "valid focus can be interacted with");
    EXPECT_EQ_INT(rt_game3d_interactor_get_interact_count(scanner),
                  INT64_MAX,
                  "interaction counter saturates instead of overflowing");

    rt_game3d_interactable_set_enabled(item, 0);
    EXPECT_TRUE(rt_game3d_interactor_get_focused(scanner) == nullptr,
                "disabled focus is invalidated between world ticks");
    EXPECT_TRUE(rt_game3d_interactor_interact(scanner) == 0,
                "disabled stale focus cannot receive an interaction");
    rt_game3d_interactable_set_enabled(item, 1);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    focused = rt_game3d_interactor_get_focused(scanner);
    EXPECT_TRUE(focused == item, "re-enabled target can reacquire focus");
    release_creation_ref(focused);
    release_creation_ref(item);
    release_creation_ref(candidate);
    rt_game3d_world_despawn(world, candidate);
    EXPECT_TRUE(rt_game3d_interactor_get_focused(scanner) == nullptr,
                "focus owner weak slot zeroes when despawn frees the entity");
    rt_game3d_world_destroy(world);
    PASS();
}

bool test_interaction_negative_priority_hysteresis() {
    TEST("Interactor3D hysteresis remains sticky for negative candidate scores");
    void *world = rt_game3d_world_new(rt_const_cstr("Interaction hysteresis"), 64, 48);
    rt_game3d_world_set_gravity(world, 0.0, 0.0, 0.0);
    void *owner = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, owner);
    void *scanner = rt_game3d_interactor_new(owner);
    rt_game3d_interactor_set_require_los(scanner, 0);
    void *first_entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(first_entity, 0.0, 0.0, -1.0);
    void *first = rt_game3d_interactable_new(first_entity);
    rt_game3d_interactable_set_focus_priority(first, -10.0);
    rt_game3d_world_spawn(world, first_entity);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    void *focused = rt_game3d_interactor_get_focused(scanner);
    EXPECT_TRUE(focused == first, "first negative-score candidate acquires focus");
    release_creation_ref(focused);

    void *second_entity = rt_game3d_entity_new();
    rt_game3d_entity_set_position(second_entity, 0.0, 0.0, -1.0);
    void *second = rt_game3d_interactable_new(second_entity);
    rt_game3d_interactable_set_focus_priority(second, -10.0);
    rt_game3d_world_spawn(world, second_entity);
    rt_game3d_world_step_simulation(world, 1.0 / 60.0);
    focused = rt_game3d_interactor_get_focused(scanner);
    EXPECT_TRUE(focused == first, "sign-safe hysteresis keeps the existing negative-score focus");
    release_creation_ref(focused);
    rt_game3d_world_destroy(world);
    PASS();
}

struct AuditMinimapMarker {
    int8_t used;
    void *entity;
    double point[3];
    void *icon;
    int64_t color;
    double scale;
    int8_t edge_clamp;
    int8_t on_compass;
    int8_t objective;
};

struct AuditMinimap {
    void *world;
    void *map_image;
    double world_min_x;
    double world_min_z;
    double world_max_x;
    double world_max_z;
    void *tracked;
    int64_t size_px;
    double view_x;
    double view_y;
    double view_w;
    double view_h;
    int8_t compass_enabled;
    double compass_width;
    AuditMinimapMarker markers[64];
    int64_t next_marker_id;
    int64_t marker_ids[64];
};

bool test_minimap_types_bounds_and_id_wrap() {
    TEST("Minimap3D validates handles/bounds and wraps marker ids without overflow");
    void *world = rt_game3d_world_new(rt_const_cstr("Minimap audit"), 64, 48);
    void *map = rt_game3d_minimap_new(world, 64);
    void *pixels = rt_pixels_new(1, 1);
    EXPECT_TRUE(expect_trap_contains(
                    [&] { rt_game3d_minimap_set_map_image(map, world, -1, -1, 1, 1); }, "Pixels"),
                "wrong-class map image is rejected");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { rt_game3d_minimap_set_map_image(map, pixels, -DBL_MAX, -1, DBL_MAX, 1); },
                    "bounded"),
                "overflow-prone map rectangle is rejected");
    EXPECT_TRUE(expect_trap_contains(
                    [&] { rt_game3d_minimap_set_viewport(map, DBL_MAX, 0, 64, 64); }, "bounded"),
                "viewport values unsafe for integer drawing are rejected");
    rt_game3d_minimap_set_map_image(map, pixels, -10.0, -10.0, 10.0, 10.0);
    EXPECT_TRUE(std::isfinite(rt_game3d_minimap_map_x(map, INFINITY, 0.0)),
                "non-finite projection input produces a finite fallback");

    void *point = rt_vec3_new(0.0, 0.0, 0.0);
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_game3d_minimap_add_marker_at(map, point, world, 0); }, "Pixels"),
                "wrong-class marker icon is rejected");
    auto *map_view = static_cast<AuditMinimap *>(map);
    map_view->next_marker_id = INT64_MAX;
    int64_t last_id = rt_game3d_minimap_add_marker_at(map, point, nullptr, 0xFFFFFF);
    int64_t wrapped_id = rt_game3d_minimap_add_marker_at(map, point, nullptr, 0xFFFFFF);
    EXPECT_EQ_INT(last_id, INT64_MAX, "maximum positive id is issued safely");
    EXPECT_EQ_INT(wrapped_id, 1, "next marker id wraps to one without signed overflow");

    void *stale = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, stale);
    rt_game3d_minimap_set_tracked_entity(map, stale);
    rt_game3d_world_despawn(world, stale);
    rt_game3d_minimap_draw(map);
    EXPECT_TRUE(map_view->tracked == nullptr,
                "drawing releases a tracked entity that was destroyed after assignment");
    EXPECT_EQ_INT(rt_game3d_minimap_add_marker(map, stale, nullptr, 0),
                  0,
                  "destroyed entities cannot become markers");

    release_creation_ref(stale);
    release_creation_ref(point);
    release_creation_ref(pixels);
    release_creation_ref(map);
    rt_game3d_world_destroy(world);
    PASS();
}

struct AuditSurfaceRow {
    void *clips[8];
    int32_t clip_count;
    double loudness;
};

struct AuditSurfaceTable {
    void *vptr;
    AuditSurfaceRow rows[256];
};

bool test_surface_table_type_and_count_guards() {
    TEST("SurfaceTable3D accepts only Sound handles and clamps private row counts");
    void *table = rt_game3d_surface_table_new();
    void *wrong = rt_game3d_entity_new();
    EXPECT_TRUE(expect_trap_contains(
                    [&] { (void)rt_game3d_surface_table_add_clip(table, 0, wrong); }, "Sound"),
                "non-Sound clip is rejected");
    EXPECT_EQ_INT(
        rt_game3d_surface_table_clip_count(table, 0), 0, "rejected clip does not mutate the row");
    auto *table_view = static_cast<AuditSurfaceTable *>(table);
    table_view->rows[0].clip_count = INT32_MAX;
    EXPECT_EQ_INT(rt_game3d_surface_table_clip_count(table, 0),
                  8,
                  "corrupt row count is clamped to fixed clip storage");
    table_view->rows[0].clip_count = 0;
    release_creation_ref(wrong);
    release_creation_ref(table);
    PASS();
}

bool test_destroyed_component_owners_fail_closed() {
    TEST("Entity-owned gameplay components reject a destroyed owner");
    rt_game3d_diagnostics_reset();
    void *world = rt_game3d_world_new(rt_const_cstr("Stale component audit"), 64, 48);
    void *entity = rt_game3d_entity_new();
    rt_game3d_world_spawn(world, entity);
    rt_game3d_world_despawn(world, entity);
    void *table = rt_game3d_surface_table_new();
    void *tree = rt_game3d_btree_new();
    int64_t root = rt_game3d_btree_wait(tree, 0.0);
    rt_game3d_btree_set_root(tree, root);
    EXPECT_TRUE(rt_game3d_perception_new(entity) == nullptr, "Perception3D rejects stale owner");
    EXPECT_TRUE(rt_game3d_interactable_new(entity) == nullptr,
                "Interactable3D rejects stale owner");
    EXPECT_TRUE(rt_game3d_interactor_new(entity) == nullptr, "Interactor3D rejects stale owner");
    EXPECT_TRUE(rt_game3d_footsteps_new(entity, table) == nullptr,
                "Footsteps3D rejects stale owner");
    EXPECT_TRUE(rt_game3d_bt_instance_new(entity, tree) == nullptr,
                "BehaviorTreeInstance3D rejects stale owner");
    EXPECT_TRUE(rt_game3d_diagnostics_get_stale_entity_calls() >= 5,
                "stale component construction is observable in diagnostics");
    release_creation_ref(entity);
    release_creation_ref(tree);
    release_creation_ref(table);
    rt_game3d_world_destroy(world);
    PASS();
}

} // namespace

int main() {
    std::printf("Game3D runtime audit regression tests\n");
    bool ok = true;
    ok = test_perception_self_occlusion_counts_and_hearing_overflow() && ok;
    ok = test_behavior_tree_topology_and_pending_bounds() && ok;
    ok = test_behavior_tree_target_weak_lifetime() && ok;
    ok = test_interaction_los_stale_focus_and_saturation() && ok;
    ok = test_interaction_negative_priority_hysteresis() && ok;
    ok = test_minimap_types_bounds_and_id_wrap() && ok;
    ok = test_surface_table_type_and_count_guards() && ok;
    ok = test_destroyed_component_owners_fail_closed() && ok;
    std::printf("\nGame3D runtime audit tests: %d/%d passed\n", g_tests_passed, g_tests_total);
    return ok && g_tests_passed == g_tests_total ? 0 : 1;
}
