//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_ai.c
// Purpose: NPC AI layers — Perception3D (sight cones with LoS raycasts and
//   enter/exit hysteresis, hearing from World3D.ReportSound stimuli) and a
//   data-built BehaviorTree3D runtime (Sequence/Selector/Inverter composites,
//   CanSee/Wait/MoveTo/Custom leaves with polled custom-leaf resolution),
//   both ticked in the world step before controllers so decisions feed the
//   same step's movement.
// Key invariants:
//   - Deterministic: world-entity-list scan order, fixed hysteresis windows,
//     no wall-clock reads; custom leaves park Running and resume on resolve.
//   - Bounded private counts fail closed; behavior-tree topology is acyclic,
//     decorators have one child, and leaf nodes cannot own children.
//   - Trees are shared immutable data; per-entity state lives in the
//     BehaviorTreeInstance attached to the entity slot.
// Ownership/Lifetime:
//   - Components hold plain entity backrefs (NULLed at teardown); instances
//     retain their tree and keep targets through zeroing weak references;
//     perception retains nothing beyond bookkeeping.
// Links: misc/plans/thirdpersonupgrade/22-ai-perception-bt.md, ADR 0094.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements Game3D perception, hearing stimuli, and behavior-tree execution.
/// @details Perception uses deterministic entity-order scans, sight hysteresis, and bounded event
///   buffers. Behavior-tree definitions are shared while timers, running-child cursors, targets,
///   and pending custom-leaf state remain local to each attached instance.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_physics3d.h"
#include "rt_scene3d.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PERCEPTION3D_MAX_TRACKED 16
#define PERCEPTION3D_MAX_HEARD 8
#define BT3D_MAX_NODES 128
#define BT3D_MAX_CHILDREN 8

/*==========================================================================
 * Perception3D
 *=========================================================================*/

typedef struct rt_game3d_percept_track {
    void *entity;      /* zeroing weak Entity3D slot */
    int64_t entity_id; /* stable id guard for logical entity identity */
    double visible_time;
    double lost_time;
    double last_known[3];
    int8_t seen;
    int8_t touched; /* set when the perception tick saw this entity alive */
} rt_game3d_percept_track;

typedef struct rt_game3d_heard_event {
    double position[3];
    double loudness;
    int64_t tag;
} rt_game3d_heard_event;

typedef struct rt_game3d_perception {
    void *vptr;
    void *entity; /* owner backref */
    double sight_range;
    double fov_degrees;
    double eye_height;
    double hearing_range; /* range at loudness 1; 0 disables hearing */
    int64_t target_mask;  /* entity layer filter */
    int64_t los_mask;
    double time_to_see;
    double time_to_lose;
    rt_game3d_percept_track tracks[PERCEPTION3D_MAX_TRACKED];
    int32_t track_count;
    rt_game3d_heard_event heard[PERCEPTION3D_MAX_HEARD];
    int32_t heard_count;
    int8_t seen_changed; /* one-shot: any seen/lost transition this step */
} rt_game3d_perception;

/// @brief Clamp a perception track count to the fixed backing-array extent.
/// @param sense Perception component whose private count is inspected.
/// @return A safe count in `[0, PERCEPTION3D_MAX_TRACKED]`.
static int32_t game3d_perception_track_count(const rt_game3d_perception *sense) {
    if (!sense || sense->track_count <= 0)
        return 0;
    return sense->track_count > PERCEPTION3D_MAX_TRACKED ? PERCEPTION3D_MAX_TRACKED
                                                         : sense->track_count;
}

/// @brief Clamp a perception heard-event count to the fixed backing-array extent.
/// @param sense Perception component whose private count is inspected.
/// @return A safe count in `[0, PERCEPTION3D_MAX_HEARD]`.
static int32_t game3d_perception_heard_count_safe(const rt_game3d_perception *sense) {
    if (!sense || sense->heard_count <= 0)
        return 0;
    return sense->heard_count > PERCEPTION3D_MAX_HEARD ? PERCEPTION3D_MAX_HEARD
                                                       : sense->heard_count;
}

/// @brief Return a fail-closed world entity count for gameplay scans.
/// @param world World whose dense entity storage is inspected.
/// @return A safe logical count, or zero for missing/corrupt storage.
static int32_t game3d_ai_world_entity_count(const rt_game3d_world *world) {
    if (!world || !world->entities || world->entity_count <= 0 || world->entity_capacity <= 0)
        return 0;
    return world->entity_count > world->entity_capacity ? world->entity_capacity
                                                        : world->entity_count;
}

/// @brief Detach a perception component from its non-owning entity back-reference.
/// @param obj Perception3D allocation being finalized.
static void game3d_perception_finalize(void *obj) {
    rt_game3d_perception *sense = (rt_game3d_perception *)obj;
    if (!sense)
        return;
    sense->entity = NULL;
    for (int32_t i = 0; i < PERCEPTION3D_MAX_TRACKED; ++i)
        rt_weak_store(&sense->tracks[i].entity, NULL);
}

/// @brief Promote and validate a perception track's zeroing weak entity slot.
/// @param track Mutable track whose weak handle is loaded.
/// @return Retained live Entity3D with the recorded stable id, or NULL.
static rt_game3d_entity *game3d_perception_track_retain_target(rt_game3d_percept_track *track) {
    rt_game3d_entity *target = track ? (rt_game3d_entity *)rt_weak_load(&track->entity) : NULL;
    if (game3d_entity_alive_or_record(target) && track->entity_id == target->id)
        return target;
    if (target)
        rt_weak_store(&track->entity, NULL);
    game3d_release_ref((void **)&target);
    return NULL;
}

/// @brief Test a track against a live entity without exposing a weak raw pointer.
/// @param track Track whose target and stable id are checked.
/// @param target Borrowed live entity candidate.
/// @return Nonzero only for the same live logical entity.
static int game3d_perception_track_matches(rt_game3d_percept_track *track,
                                           const rt_game3d_entity *target) {
    if (!track || !target || track->entity_id != target->id)
        return 0;
    rt_game3d_entity *tracked = game3d_perception_track_retain_target(track);
    int matches = tracked && tracked == target;
    game3d_release_ref((void **)&tracked);
    return matches;
}

/// @brief Allocate a perception component and attach it to an entity.
/// @details The component starts with a 15-unit, 110-degree sight cone and all-layer masks.
/// @param entity_obj Entity3D that will own the component.
/// @return A new Perception3D handle, or NULL when validation or allocation fails.
void *rt_game3d_perception_new(void *entity_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.Perception3D.New: invalid entity");
    if (!entity) {
        return NULL;
    }
    rt_game3d_perception *sense = (rt_game3d_perception *)rt_obj_new_i64(
        RT_G3D_GAME3D_PERCEPTION_CLASS_ID, (int64_t)sizeof(rt_game3d_perception));
    if (!sense) {
        rt_trap("Game3D.Perception3D.New: allocation failed");
        return NULL;
    }
    memset(sense, 0, sizeof(*sense));
    rt_obj_set_finalizer(sense, game3d_perception_finalize);
    sense->entity = entity;
    sense->sight_range = 15.0;
    sense->fov_degrees = 110.0;
    sense->eye_height = 1.6;
    sense->target_mask = -1;
    sense->los_mask = -1;
    sense->time_to_see = 0.3;
    sense->time_to_lose = 2.0;
    {
        rt_game3d_perception *previous = (rt_game3d_perception *)rt_g3d_checked_or_null(
            entity->perception, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
        if (previous && previous != sense)
            previous->entity = NULL;
        game3d_assign_typed_ref(&entity->perception, sense, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
    }
    return sense;
}

/// @brief Validate and cast an opaque handle to a perception component.
/// @param obj Candidate Perception3D handle.
/// @param method Trap message emitted when @p obj has the wrong runtime class.
/// @return The validated component, or NULL after reporting an invalid handle.
static rt_game3d_perception *game3d_perception_checked(void *obj, const char *method) {
    rt_game3d_perception *sense =
        (rt_game3d_perception *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
    if (!sense)
        rt_trap(method);
    return sense;
}

/// @brief Configure a perception component's sight range, cone, and eye offset.
/// @param obj Perception3D component to configure.
/// @param range Positive sight distance, clamped to 512 world units.
/// @param fov_degrees Full cone angle, clamped to 360 degrees.
/// @param eye_height Finite vertical offset applied to the owner's world position.
void rt_game3d_perception_set_sight(void *obj,
                                    double range,
                                    double fov_degrees,
                                    double eye_height) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SetSight: invalid component");
    if (!sense)
        return;
    if (isfinite(range) && range > 0.0)
        sense->sight_range = range > 512.0 ? 512.0 : range;
    if (isfinite(fov_degrees) && fov_degrees > 1.0)
        sense->fov_degrees = fov_degrees > 360.0 ? 360.0 : fov_degrees;
    if (isfinite(eye_height))
        sense->eye_height =
            game3d_clamp_abs_or(eye_height, sense->eye_height, RT_GAME3D_COORD_ABS_MAX);
}

/// @brief Configure the range at which a unit-loudness sound is audible.
/// @param obj Perception3D component to configure.
/// @param range_at_loudness1 Non-negative base range, clamped to 512 world units.
void rt_game3d_perception_set_hearing(void *obj, double range_at_loudness1) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SetHearing: invalid component");
    if (sense && isfinite(range_at_loudness1) && range_at_loudness1 >= 0.0)
        sense->hearing_range = range_at_loudness1 > 512.0 ? 512.0 : range_at_loudness1;
}

/// @brief Set the entity-layer mask eligible for visual tracking.
/// @param obj Perception3D component to configure.
/// @param mask Bit mask tested against each candidate entity's layer.
void rt_game3d_perception_set_target_mask(void *obj, int64_t mask) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SetTargetMask: invalid component");
    if (sense)
        sense->target_mask = mask;
}

/// @brief Set the collision-layer mask used by sight occlusion raycasts.
/// @param obj Perception3D component to configure.
/// @param mask Bit mask of layers that can block line of sight.
void rt_game3d_perception_set_los_mask(void *obj, int64_t mask) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SetLosMask: invalid component");
    if (sense)
        sense->los_mask = mask;
}

/// @brief Count tracks whose sight hysteresis currently marks them visible.
/// @param obj Perception3D component to query.
/// @return The number of visible tracks, or 0 for an invalid component.
int64_t rt_game3d_perception_seen_count(void *obj) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SeenCount: invalid component");
    if (!sense)
        return 0;
    int64_t count = 0;
    int32_t track_count = game3d_perception_track_count(sense);
    for (int32_t i = 0; i < track_count; ++i) {
        if (!sense->tracks[i].seen)
            continue;
        rt_game3d_entity *target = game3d_perception_track_retain_target(&sense->tracks[i]);
        if (target)
            count++;
        game3d_release_ref((void **)&target);
    }
    return count;
}

/// @brief Retrieve a live target by its compact visible-list index.
/// @param obj Perception3D component to query.
/// @param index Zero-based index among tracks currently marked visible.
/// @return A retained Entity3D handle, or NULL when the index is invalid or the target died.
void *rt_game3d_perception_seen_target(void *obj, int64_t index) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SeenTarget: invalid component");
    if (!sense || index < 0)
        return NULL;
    int64_t seen_index = 0;
    int32_t track_count = game3d_perception_track_count(sense);
    for (int32_t i = 0; i < track_count; ++i) {
        if (!sense->tracks[i].seen)
            continue;
        rt_game3d_entity *target = game3d_perception_track_retain_target(&sense->tracks[i]);
        if (!target)
            continue;
        if (seen_index == index)
            return target;
        game3d_release_ref((void **)&target);
        seen_index++;
    }
    return NULL;
}

/// @brief Last world position where @p target was seen (its live position while seen).
/// @param obj Perception3D component containing the target tracks.
/// @param target Entity3D whose track should be queried.
/// @return A new Vec3 containing the last-known position, or the zero vector when unavailable.
void *rt_game3d_perception_last_known_position(void *obj, void *target) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.LastKnownPosition: invalid component");
    if (!sense)
        return rt_vec3_new(0.0, 0.0, 0.0);
    rt_game3d_entity *target_entity =
        target
            ? game3d_entity_checked(
                  target, "Game3D.Perception3D.LastKnownPosition: target must be a live Entity3D")
            : NULL;
    if (!target_entity)
        return rt_vec3_new(0.0, 0.0, 0.0);
    int32_t track_count = game3d_perception_track_count(sense);
    for (int32_t i = 0; i < track_count; ++i) {
        if (game3d_perception_track_matches(&sense->tracks[i], target_entity))
            return rt_vec3_new(sense->tracks[i].last_known[0],
                               sense->tracks[i].last_known[1],
                               sense->tracks[i].last_known[2]);
    }
    return rt_vec3_new(0.0, 0.0, 0.0);
}

/// @brief One-shot: any seen/lost transition since the last call.
/// @param obj Perception3D component whose change flag is consumed.
/// @return 1 when any track changed visibility, or 0 when unchanged or invalid.
int8_t rt_game3d_perception_seen_changed(void *obj) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.SeenChanged: invalid component");
    if (!sense)
        return 0;
    int8_t changed = sense->seen_changed;
    sense->seen_changed = 0;
    return changed;
}

/// @brief Count sound stimuli buffered for the current perception interval.
/// @param obj Perception3D component to query.
/// @return The buffered event count, or 0 for an invalid component.
int64_t rt_game3d_perception_heard_count(void *obj) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.HeardCount: invalid component");
    return game3d_perception_heard_count_safe(sense);
}

/// @brief Retrieve the world-space origin of a buffered sound stimulus.
/// @param obj Perception3D component to query.
/// @param index Zero-based heard-event index.
/// @return A new Vec3 containing the event position, or the zero vector when out of range.
void *rt_game3d_perception_heard_position(void *obj, int64_t index) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.HeardPosition: invalid component");
    if (!sense || index < 0 || index >= game3d_perception_heard_count_safe(sense))
        return rt_vec3_new(0.0, 0.0, 0.0);
    return rt_vec3_new(sense->heard[index].position[0],
                       sense->heard[index].position[1],
                       sense->heard[index].position[2]);
}

/// @brief Retrieve the application-defined tag of a buffered sound stimulus.
/// @param obj Perception3D component to query.
/// @param index Zero-based heard-event index.
/// @return The event tag, or 0 when the index or component is invalid.
int64_t rt_game3d_perception_heard_tag(void *obj, int64_t index) {
    rt_game3d_perception *sense =
        game3d_perception_checked(obj, "Game3D.Perception3D.HeardTag: invalid component");
    if (!sense || index < 0 || index >= game3d_perception_heard_count_safe(sense))
        return 0;
    return sense->heard[index].tag;
}

/// @brief Find the existing track slot for @p target, or NULL.
/// @details Slots key on the entity pointer AND its stable id, so a freed
///   entity's heap address reused by a new entity starts from a fresh track
///   instead of inheriting stale seen/last_known state.
/// @param sense Perception component whose bounded track table is searched.
/// @param target Live entity and stable identifier to match.
/// @return The mutable matching track, or NULL when the entity is not tracked.
static rt_game3d_percept_track *game3d_perception_find_track(rt_game3d_perception *sense,
                                                             const rt_game3d_entity *target) {
    int32_t track_count = game3d_perception_track_count(sense);
    for (int32_t i = 0; i < track_count; ++i)
        if (game3d_perception_track_matches(&sense->tracks[i], target))
            return &sense->tracks[i];
    return NULL;
}

/// @brief Create a fresh track slot for @p target (NULL when the table is full).
/// @details Tracks are created lazily — only when a target first becomes
///   visible — so distant never-seen entities cannot hog slots. Dead tracks are
///   compacted away at the end of every perception tick, so the table cannot
///   fill permanently.
/// @param sense Perception component whose track table receives the entry.
/// @param target Live entity associated with the new entry.
/// @return The zero-initialized track, or NULL when all track slots are occupied.
static rt_game3d_percept_track *game3d_perception_create_track(rt_game3d_perception *sense,
                                                               rt_game3d_entity *target) {
    if (sense->track_count < 0)
        sense->track_count = 0;
    else if (sense->track_count > PERCEPTION3D_MAX_TRACKED)
        sense->track_count = PERCEPTION3D_MAX_TRACKED;
    if (sense->track_count >= PERCEPTION3D_MAX_TRACKED)
        return NULL;
    rt_game3d_percept_track *track = &sense->tracks[sense->track_count];
    rt_weak_store(&track->entity, NULL);
    memset(track, 0, sizeof(*track));
    rt_weak_store(&track->entity, target);
    if (!track->entity)
        return NULL;
    track->entity_id = target->id;
    sense->track_count++;
    return track;
}

/// @brief Drop tracks that the current tick did not touch (dead/removed/filtered
///   entities), compacting the table so slots are always reclaimable.
/// @param sense Perception component whose track table is compacted in place.
static void game3d_perception_compact_tracks(rt_game3d_perception *sense) {
    int32_t kept = 0;
    int32_t track_count = game3d_perception_track_count(sense);
    for (int32_t i = 0; i < track_count; ++i) {
        if (!sense->tracks[i].touched) {
            rt_weak_store(&sense->tracks[i].entity, NULL);
            memset(&sense->tracks[i], 0, sizeof(sense->tracks[i]));
            continue;
        }
        if (kept != i) {
            sense->tracks[kept] = sense->tracks[i];
            memset(&sense->tracks[i], 0, sizeof(sense->tracks[i]));
        }
        kept++;
    }
    sense->track_count = kept;
}

/// @brief Per-step sight update (cone + LoS + hysteresis) for one perceiver.
/// @details The scan follows world entity order, refreshes last-known positions, and expires the
///   previous step's heard-event buffer before new stimuli are reported.
/// @param world World3D containing candidate targets and the physics query interface.
/// @param owner Entity3D that owns the perception component.
/// @param dt Deterministic simulation interval in seconds.
void game3d_perception_tick(rt_game3d_world *world, rt_game3d_entity *owner, double dt) {
    rt_game3d_perception *sense = (rt_game3d_perception *)rt_g3d_checked_or_null(
        owner->perception, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
    if (!sense)
        return;
    /* Heard events expire every step; World3D.ReportSound refills them. */
    sense->heard_count = 0;
    /* Tracks touched by this tick survive; the rest are compacted away below. */
    sense->track_count = game3d_perception_track_count(sense);
    for (int32_t i = 0; i < sense->track_count; ++i)
        sense->tracks[i].touched = 0;

    double origin[3];
    if (!game3d_entity_world_position_components(owner, origin))
        return;
    origin[1] =
        game3d_clamp_abs_or(origin[1] + sense->eye_height, origin[1], RT_GAME3D_COORD_ABS_MAX);
    double forward[3] = {0.0, 0.0, -1.0};
    if (owner->node) {
        double qx, qy, qz, qw;
        if (rt_scene_node3d_get_world_rotation_components(owner->node, &qx, &qy, &qz, &qw)) {
            double x = 0.0, y = 0.0, z = -1.0;
            double tx = 2.0 * (qy * z - qz * y);
            double ty = 2.0 * (qz * x - qx * z);
            double tz = 2.0 * (qx * y - qy * x);
            forward[0] = x + qw * tx + (qy * tz - qz * ty);
            forward[1] = y + qw * ty + (qz * tx - qx * tz);
            forward[2] = z + qw * tz + (qx * ty - qy * tx);
        }
    }
    double cone_cos = cos(sense->fov_degrees * 0.5 * (3.14159265358979323846 / 180.0));

    int32_t count = game3d_ai_world_entity_count(world);
    double sight_range_sq = sense->sight_range * sense->sight_range;
    for (int32_t i = 0; i < count; ++i) {
        rt_game3d_entity *target = world->entities ? world->entities[i] : NULL;
        if (!target || !target->alive || target == owner)
            continue;
        if (sense->target_mask != -1 && (target->layer & sense->target_mask) == 0)
            continue;
        rt_game3d_percept_track *track = game3d_perception_find_track(sense, target);
        /* An untracked target with a full table can neither be recorded nor
         * transition any state: skip all remaining work for it. */
        if (!track && sense->track_count >= PERCEPTION3D_MAX_TRACKED)
            continue;
        double tpos[3];
        if (!game3d_entity_world_position_components(target, tpos))
            continue;
        double to[3] = {tpos[0] - origin[0], tpos[1] - origin[1], tpos[2] - origin[2]};
        double dist_sq = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
        int visible = 0;
        if (isfinite(dist_sq) && dist_sq <= sight_range_sq && dist_sq > 1e-12) {
            double dist = sqrt(dist_sq);
            double align = (to[0] * forward[0] + to[1] * forward[1] + to[2] * forward[2]) / dist;
            if (align >= cone_cos) {
                visible = 1;
                /* Skip the occlusion ray for near-touching targets: the epsilon
                 * pull-back would drive the ray length negative. The raw
                 * closest-body raycast is allocation-free — the old boxed path
                 * created two Vec3 handles plus a hit object per in-cone
                 * target per perceiver per step. */
                if (world->physics && dist > 0.05) {
                    void *owner_body = game3d_entity_body_ref(owner);
                    void *target_body = game3d_entity_body_ref(target);
                    void *hit_body = rt_world3d_raycast_closest_body_raw(world->physics,
                                                                         origin[0],
                                                                         origin[1],
                                                                         origin[2],
                                                                         to[0] / dist,
                                                                         to[1] / dist,
                                                                         to[2] / dist,
                                                                         dist - 0.05,
                                                                         sense->los_mask,
                                                                         owner_body,
                                                                         NULL);
                    if (hit_body && hit_body != target_body)
                        visible = 0;
                }
            }
        }
        /* Tracks begin at first visibility; invisible untracked targets carry
         * no state worth storing. */
        if (!track && visible)
            track = game3d_perception_create_track(sense, target);
        if (!track)
            continue;
        track->touched = 1;
        if (visible) {
            track->visible_time += dt;
            track->lost_time = 0.0;
            memcpy(track->last_known, tpos, sizeof(track->last_known));
            if (!track->seen && track->visible_time >= sense->time_to_see) {
                track->seen = 1;
                sense->seen_changed = 1;
            }
        } else {
            track->visible_time = 0.0;
            track->lost_time += dt;
            if (track->seen && track->lost_time >= sense->time_to_lose) {
                track->seen = 0;
                sense->seen_changed = 1;
            }
        }
    }
    game3d_perception_compact_tracks(sense);
}

/// @brief World stimulus: deliver a sound event to every hearing perceiver in range.
/// @details A listener's effective range is its unit-loudness range multiplied by @p loudness;
///   listeners with full bounded event buffers skip the stimulus.
/// @param world_obj World3D whose live perception components receive the stimulus.
/// @param position Vec3 containing the sound's world-space origin.
/// @param loudness Positive finite loudness multiplier.
/// @param tag Application-defined value copied into each delivered event.
void rt_game3d_world_report_sound(void *world_obj, void *position, double loudness, int64_t tag) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.World3D.ReportSound: invalid world");
    if (!world)
        return;
    double pos[3];
    if (!game3d_read_vec3(position, pos, "Game3D.World3D.ReportSound: position must be Vec3"))
        return;
    if (!isfinite(loudness) || loudness <= 0.0)
        return;
    int32_t count = game3d_ai_world_entity_count(world);
    for (int32_t i = 0; i < count; ++i) {
        rt_game3d_entity *entity = world->entities ? world->entities[i] : NULL;
        if (!entity || !entity->alive || !entity->perception)
            continue;
        rt_game3d_perception *sense = (rt_game3d_perception *)rt_g3d_checked_or_null(
            entity->perception, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
        if (!sense || sense->hearing_range <= 0.0)
            continue;
        sense->heard_count = game3d_perception_heard_count_safe(sense);
        if (sense->heard_count >= PERCEPTION3D_MAX_HEARD)
            continue;
        double epos[3];
        if (!game3d_entity_world_position_components(entity, epos))
            continue;
        double dx = pos[0] - epos[0];
        double dy = pos[1] - epos[1];
        double dz = pos[2] - epos[2];
        /* Squared-distance compare: skips a sqrt per entity per sound event. */
        double dist_sq = dx * dx + dy * dy + dz * dz;
        double reach = sense->hearing_range * loudness;
        if (!isfinite(dist_sq) || reach <= 0.0 || (isfinite(reach) && dist_sq > reach * reach))
            continue;
        rt_game3d_heard_event *event = &sense->heard[sense->heard_count++];
        memcpy(event->position, pos, sizeof(event->position));
        event->loudness = loudness;
        event->tag = tag;
    }
}

/*==========================================================================
 * BehaviorTree3D — shared immutable tree + per-entity instance
 *=========================================================================*/

enum {
    BT3D_NODE_SEQUENCE = 0,
    BT3D_NODE_SELECTOR = 1,
    BT3D_NODE_INVERTER = 2,
    BT3D_NODE_CAN_SEE = 3,
    BT3D_NODE_WAIT = 4,
    BT3D_NODE_MOVE_TO_TARGET = 5,
    BT3D_NODE_MOVE_TO_LAST_KNOWN = 6,
    BT3D_NODE_CUSTOM = 7,
};

enum {
    BT3D_FAILURE = 0,
    BT3D_SUCCESS = 1,
    BT3D_RUNNING = 2,
};

typedef struct rt_game3d_bt_node {
    int32_t type;
    int32_t children[BT3D_MAX_CHILDREN];
    int32_t child_count;
    double p0;  /* Wait seconds / MoveTo speed */
    double p1;  /* MoveTo arrive distance */
    int64_t i0; /* Custom id */
} rt_game3d_bt_node;

typedef struct rt_game3d_btree {
    void *vptr;
    rt_game3d_bt_node nodes[BT3D_MAX_NODES];
    int32_t node_count;
    int32_t root;
} rt_game3d_btree;

typedef struct rt_game3d_bt_instance {
    void *vptr;
    void *entity; /* owner backref */
    void *tree;   /* retained BehaviorTree3D */
    void *target; /* zeroing weak Entity3D slot */
    double timers[BT3D_MAX_NODES];
    int32_t running_child[BT3D_MAX_NODES];
    int8_t custom_pending[BT3D_MAX_NODES];
    int8_t custom_result[BT3D_MAX_NODES]; /* 0 none, 1 success, 2 failure */
    int64_t pending_custom_id;            /* 0 = none; polled by the game */
    int32_t pending_custom_node;
} rt_game3d_bt_instance;

/// @brief Finalize a behavior-tree definition that owns no external resources.
/// @param obj BehaviorTree3D allocation being finalized.
static void game3d_btree_finalize(void *obj) {
    (void)obj;
}

/// @brief Allocate an empty behavior-tree definition with no selected root.
/// @return A new BehaviorTree3D handle, or NULL when allocation fails.
void *rt_game3d_btree_new(void) {
    rt_game3d_btree *tree = (rt_game3d_btree *)rt_obj_new_i64(RT_G3D_GAME3D_BTREE_CLASS_ID,
                                                              (int64_t)sizeof(rt_game3d_btree));
    if (!tree) {
        rt_trap("Game3D.BehaviorTree3D.New: allocation failed");
        return NULL;
    }
    memset(tree, 0, sizeof(*tree));
    rt_obj_set_finalizer(tree, game3d_btree_finalize);
    tree->root = -1;
    return tree;
}

/// @brief Validate and cast an opaque handle to a behavior-tree definition.
/// @param obj Candidate BehaviorTree3D handle.
/// @param method Trap message emitted when @p obj has the wrong runtime class.
/// @return The validated tree, or NULL after reporting an invalid handle.
static rt_game3d_btree *game3d_btree_checked(void *obj, const char *method) {
    rt_game3d_btree *tree =
        (rt_game3d_btree *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_BTREE_CLASS_ID);
    if (!tree)
        rt_trap(method);
    return tree;
}

/// @brief Append a zero-initialized node of the requested internal type.
/// @param tree BehaviorTree3D definition to extend.
/// @param type One of the internal BT3D_NODE_* discriminants.
/// @return The new node index, or -1 after reporting an invalid or full tree.
static int64_t game3d_btree_add_node(rt_game3d_btree *tree, int32_t type) {
    if (!tree || tree->node_count < 0 || tree->node_count >= BT3D_MAX_NODES) {
        rt_trap("Game3D.BehaviorTree3D: node budget (128) exceeded");
        return -1;
    }
    rt_game3d_bt_node *node = &tree->nodes[tree->node_count];
    memset(node, 0, sizeof(*node));
    node->type = type;
    return tree->node_count++;
}

/// @brief Append a sequence composite that fails on its first failing child.
/// @param obj BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_sequence(void *obj) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.Sequence: invalid tree");
    return tree ? game3d_btree_add_node(tree, BT3D_NODE_SEQUENCE) : -1;
}

/// @brief Append a selector composite that succeeds on its first successful child.
/// @param obj BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_selector(void *obj) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.Selector: invalid tree");
    return tree ? game3d_btree_add_node(tree, BT3D_NODE_SELECTOR) : -1;
}

/// @brief Append a decorator that swaps its child's success and failure results.
/// @param obj BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_inverter(void *obj) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.Inverter: invalid tree");
    return tree ? game3d_btree_add_node(tree, BT3D_NODE_INVERTER) : -1;
}

/// @brief Append a leaf that tests whether the instance target is currently visible.
/// @param obj BehaviorTree3D definition to extend.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_can_see(void *obj) {
    rt_game3d_btree *tree = game3d_btree_checked(obj, "Game3D.BehaviorTree3D.CanSee: invalid tree");
    return tree ? game3d_btree_add_node(tree, BT3D_NODE_CAN_SEE) : -1;
}

/// @brief Append a leaf that remains running for a configured duration.
/// @param obj BehaviorTree3D definition to extend.
/// @param seconds Positive finite wait duration; other values produce an immediate wait.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_wait(void *obj, double seconds) {
    rt_game3d_btree *tree = game3d_btree_checked(obj, "Game3D.BehaviorTree3D.Wait: invalid tree");
    if (!tree)
        return -1;
    int64_t node = game3d_btree_add_node(tree, BT3D_NODE_WAIT);
    if (node >= 0)
        tree->nodes[node].p0 = isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
    return node;
}

/// @brief Append a leaf that moves the owner toward the current target entity.
/// @param obj BehaviorTree3D definition to extend.
/// @param speed Positive movement speed, or an invalid value to use 2 world units per second.
/// @param arrive_distance Positive success radius, or an invalid value to use 0.5 world units.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_move_to_target(void *obj, double speed, double arrive_distance) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.MoveToTarget: invalid tree");
    if (!tree)
        return -1;
    int64_t node = game3d_btree_add_node(tree, BT3D_NODE_MOVE_TO_TARGET);
    if (node >= 0) {
        tree->nodes[node].p0 = game3d_positive_clamped_or(speed, 2.0, RT_GAME3D_COORD_ABS_MAX);
        tree->nodes[node].p1 =
            game3d_positive_clamped_or(arrive_distance, 0.5, RT_GAME3D_COORD_ABS_MAX);
    }
    return node;
}

/// @brief Append a leaf that moves the owner toward its target's last-known position.
/// @param obj BehaviorTree3D definition to extend.
/// @param speed Positive movement speed, or an invalid value to use 2 world units per second.
/// @param arrive_distance Positive success radius, or an invalid value to use 0.5 world units.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_move_to_last_known(void *obj, double speed, double arrive_distance) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.MoveToLastKnown: invalid tree");
    if (!tree)
        return -1;
    int64_t node = game3d_btree_add_node(tree, BT3D_NODE_MOVE_TO_LAST_KNOWN);
    if (node >= 0) {
        tree->nodes[node].p0 = game3d_positive_clamped_or(speed, 2.0, RT_GAME3D_COORD_ABS_MAX);
        tree->nodes[node].p1 =
            game3d_positive_clamped_or(arrive_distance, 0.5, RT_GAME3D_COORD_ABS_MAX);
    }
    return node;
}

/// @brief Append a leaf whose completion is polled and resolved by application code.
/// @param obj BehaviorTree3D definition to extend.
/// @param id Application-defined identifier exposed while the leaf is pending.
/// @return The new node index, or -1 when the tree is invalid or full.
int64_t rt_game3d_btree_custom(void *obj, int64_t id) {
    rt_game3d_btree *tree = game3d_btree_checked(obj, "Game3D.BehaviorTree3D.Custom: invalid tree");
    if (!tree)
        return -1;
    if (id == 0) {
        rt_trap("Game3D.BehaviorTree3D.Custom: id zero is reserved for no pending leaf");
        return -1;
    }
    int64_t node = game3d_btree_add_node(tree, BT3D_NODE_CUSTOM);
    if (node >= 0)
        tree->nodes[node].i0 = id;
    return node;
}

/// @brief Test whether @p start already reaches @p sought through child edges.
/// @details The iterative fixed-budget traversal also fails closed when private
///          node or child counts are corrupt.
/// @param tree Behavior tree whose existing edges are traversed.
/// @param start First node to visit.
/// @param sought Node whose reachability would close a cycle.
/// @return Nonzero when reachable or when corrupt topology cannot be proven safe.
static int game3d_btree_reaches(const rt_game3d_btree *tree, int32_t start, int32_t sought) {
    if (!tree || tree->node_count < 0 || tree->node_count > BT3D_MAX_NODES || start < 0 ||
        start >= tree->node_count || sought < 0 || sought >= tree->node_count)
        return 1;
    int32_t stack[BT3D_MAX_NODES];
    int32_t stack_count = 0;
    uint8_t visited[BT3D_MAX_NODES];
    memset(visited, 0, sizeof(visited));
    stack[stack_count++] = start;
    visited[start] = 1;
    while (stack_count > 0) {
        int32_t current = stack[--stack_count];
        if (current == sought)
            return 1;
        const rt_game3d_bt_node *node = &tree->nodes[current];
        if (node->child_count < 0 || node->child_count > BT3D_MAX_CHILDREN)
            return 1;
        for (int32_t i = 0; i < node->child_count; ++i) {
            int32_t child = node->children[i];
            if (child < 0 || child >= tree->node_count)
                return 1;
            if (!visited[child]) {
                if (stack_count >= BT3D_MAX_NODES)
                    return 1;
                visited[child] = 1;
                stack[stack_count++] = child;
            }
        }
    }
    return 0;
}

/// @brief Append a child index to a composite or decorator node.
/// @details Relationships that would create a cycle, duplicate an edge, add a
///          child to a leaf, or overfill an inverter are rejected with a trap.
/// @param obj BehaviorTree3D containing both nodes.
/// @param parent Index of the node that will own the child.
/// @param child Index of the node to append.
void rt_game3d_btree_add_child(void *obj, int64_t parent, int64_t child) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.AddChild: invalid tree");
    if (!tree || parent < 0 || parent >= tree->node_count || child < 0 || child >= tree->node_count)
        return;
    if (parent == child) {
        rt_trap("Game3D.BehaviorTree3D.AddChild: relationship would create a cycle");
        return;
    }
    rt_game3d_bt_node *node = &tree->nodes[parent];
    if (node->type != BT3D_NODE_SEQUENCE && node->type != BT3D_NODE_SELECTOR &&
        node->type != BT3D_NODE_INVERTER) {
        rt_trap("Game3D.BehaviorTree3D.AddChild: leaf nodes cannot own children");
        return;
    }
    if (node->child_count < 0 || node->child_count > BT3D_MAX_CHILDREN) {
        rt_trap("Game3D.BehaviorTree3D.AddChild: corrupt child count");
        return;
    }
    for (int32_t i = 0; i < node->child_count; ++i) {
        if (node->children[i] == child) {
            rt_trap("Game3D.BehaviorTree3D.AddChild: duplicate child relationship");
            return;
        }
    }
    if (node->type == BT3D_NODE_INVERTER && node->child_count >= 1) {
        rt_trap("Game3D.BehaviorTree3D.AddChild: inverter accepts exactly one child");
        return;
    }
    if (game3d_btree_reaches(tree, (int32_t)child, (int32_t)parent)) {
        rt_trap("Game3D.BehaviorTree3D.AddChild: relationship would create a cycle");
        return;
    }
    if (node->child_count >= BT3D_MAX_CHILDREN) {
        rt_trap("Game3D.BehaviorTree3D.AddChild: child budget (8) exceeded");
        return;
    }
    node->children[node->child_count++] = (int32_t)child;
}

/// @brief Select the node at which behavior-tree evaluation begins.
/// @param obj BehaviorTree3D definition to configure.
/// @param node Valid node index to use as the root.
void rt_game3d_btree_set_root(void *obj, int64_t node) {
    rt_game3d_btree *tree =
        game3d_btree_checked(obj, "Game3D.BehaviorTree3D.SetRoot: invalid tree");
    if (tree && node >= 0 && node < tree->node_count)
        tree->root = (int32_t)node;
}

/*--------------------------------------------------------------------------
 * Instance
 *-------------------------------------------------------------------------*/

/// @brief Detach an instance from its entity and release its retained tree.
/// @param obj BehaviorTreeInstance3D allocation being finalized.
static void game3d_bt_instance_finalize(void *obj) {
    rt_game3d_bt_instance *instance = (rt_game3d_bt_instance *)obj;
    if (!instance)
        return;
    instance->entity = NULL;
    rt_weak_store(&instance->target, NULL);
    game3d_release_ref(&instance->tree);
}

/// @brief Allocate per-entity execution state for a rooted behavior tree.
/// @param entity_obj Entity3D that will own and tick the instance.
/// @param tree_obj Rooted BehaviorTree3D definition retained by the instance.
/// @return A new BehaviorTreeInstance3D handle, or NULL when validation or allocation fails.
void *rt_game3d_bt_instance_new(void *entity_obj, void *tree_obj) {
    rt_game3d_entity *entity =
        game3d_entity_checked(entity_obj, "Game3D.BehaviorTreeInstance3D.New: invalid entity");
    rt_game3d_btree *tree =
        (rt_game3d_btree *)rt_g3d_checked_or_null(tree_obj, RT_G3D_GAME3D_BTREE_CLASS_ID);
    if (!entity)
        return NULL;
    if (!tree || tree->node_count <= 0 || tree->node_count > BT3D_MAX_NODES || tree->root < 0 ||
        tree->root >= tree->node_count) {
        rt_trap("Game3D.BehaviorTreeInstance3D.New: entity and a rooted tree required");
        return NULL;
    }
    rt_game3d_bt_instance *instance = (rt_game3d_bt_instance *)rt_obj_new_i64(
        RT_G3D_GAME3D_BTINSTANCE_CLASS_ID, (int64_t)sizeof(rt_game3d_bt_instance));
    if (!instance) {
        rt_trap("Game3D.BehaviorTreeInstance3D.New: allocation failed");
        return NULL;
    }
    memset(instance, 0, sizeof(*instance));
    rt_obj_set_finalizer(instance, game3d_bt_instance_finalize);
    instance->entity = entity;
    rt_obj_retain_maybe(tree_obj);
    instance->tree = tree_obj;
    instance->pending_custom_node = -1;
    {
        rt_game3d_bt_instance *previous = (rt_game3d_bt_instance *)rt_g3d_checked_or_null(
            entity->btree, RT_G3D_GAME3D_BTINSTANCE_CLASS_ID);
        if (previous && previous != instance)
            previous->entity = NULL;
        game3d_assign_typed_ref(&entity->btree, instance, RT_G3D_GAME3D_BTINSTANCE_CLASS_ID);
    }
    return instance;
}

/// @brief Validate and cast an opaque handle to behavior-tree instance state.
/// @param obj Candidate BehaviorTreeInstance3D handle.
/// @param method Trap message emitted when @p obj has the wrong runtime class.
/// @return The validated instance, or NULL after reporting an invalid handle.
static rt_game3d_bt_instance *game3d_bt_instance_checked(void *obj, const char *method) {
    rt_game3d_bt_instance *instance =
        (rt_game3d_bt_instance *)rt_g3d_checked_or_null(obj, RT_G3D_GAME3D_BTINSTANCE_CLASS_ID);
    if (!instance)
        rt_trap(method);
    return instance;
}

/// @brief Set or clear the entity consumed by target-aware behavior leaves.
/// @param obj BehaviorTreeInstance3D to configure.
/// @param target_entity Candidate Entity3D handle, or NULL to clear the target.
void rt_game3d_bt_instance_set_target(void *obj, void *target_entity) {
    rt_game3d_bt_instance *instance = game3d_bt_instance_checked(
        obj, "Game3D.BehaviorTreeInstance3D.SetTarget: invalid instance");
    if (!instance)
        return;
    if (!target_entity) {
        rt_weak_store(&instance->target, NULL);
        return;
    }
    rt_game3d_entity *target = game3d_entity_checked(
        target_entity, "Game3D.BehaviorTreeInstance3D.SetTarget: target must be a live Entity3D");
    if (!target)
        return;
    rt_weak_store(&instance->target, target);
}

/// @brief Promote and validate an instance's zeroing weak target reference.
/// @param instance Behavior-tree instance whose target is requested.
/// @return A retained live Entity3D, or NULL when the target was cleared or destroyed.
static rt_game3d_entity *game3d_bt_instance_retain_target(rt_game3d_bt_instance *instance) {
    rt_game3d_entity *target =
        instance ? (rt_game3d_entity *)rt_weak_load(&instance->target) : NULL;
    if (game3d_entity_alive_or_record(target))
        return target;
    if (target)
        rt_weak_store(&instance->target, NULL);
    game3d_release_ref((void **)&target);
    return NULL;
}

/// @brief Validate and, when necessary, clear the single pending custom-leaf slot.
/// @param instance Behavior-tree instance whose private pending state is inspected.
/// @return Nonzero only when the pending node, id, type, and per-node flag agree.
static int game3d_bt_instance_pending_valid(rt_game3d_bt_instance *instance) {
    if (!instance)
        return 0;
    int32_t node = instance->pending_custom_node;
    rt_game3d_btree *tree =
        (rt_game3d_btree *)rt_g3d_checked_or_null(instance->tree, RT_G3D_GAME3D_BTREE_CLASS_ID);
    int valid = instance->pending_custom_id != 0 && node >= 0 && node < BT3D_MAX_NODES && tree &&
                tree->node_count > 0 && tree->node_count <= BT3D_MAX_NODES &&
                node < tree->node_count && tree->nodes[node].type == BT3D_NODE_CUSTOM &&
                instance->custom_pending[node];
    if (valid)
        return 1;
    memset(instance->custom_pending, 0, sizeof(instance->custom_pending));
    instance->pending_custom_id = 0;
    instance->pending_custom_node = -1;
    return 0;
}

/// @brief Pending Custom-leaf id awaiting game resolution (0 = none).
/// @param obj BehaviorTreeInstance3D to query.
/// @return The pending application identifier, or 0 when no custom leaf is waiting.
int64_t rt_game3d_bt_instance_pending_custom(void *obj) {
    rt_game3d_bt_instance *instance = game3d_bt_instance_checked(
        obj, "Game3D.BehaviorTreeInstance3D.get_PendingCustom: invalid instance");
    return game3d_bt_instance_pending_valid(instance) ? instance->pending_custom_id : 0;
}

/// @brief Resolve the pending Custom leaf (1 = success, 0 = failure).
/// @param obj BehaviorTreeInstance3D containing the pending custom leaf.
/// @param success Non-zero to return success from the leaf; zero to return failure.
void rt_game3d_bt_instance_resolve(void *obj, int8_t success) {
    rt_game3d_bt_instance *instance =
        game3d_bt_instance_checked(obj, "Game3D.BehaviorTreeInstance3D.Resolve: invalid instance");
    if (!game3d_bt_instance_pending_valid(instance))
        return;
    int32_t node = instance->pending_custom_node;
    instance->custom_result[node] = success ? 1 : 2;
    instance->custom_pending[node] = 0;
    instance->pending_custom_id = 0;
    instance->pending_custom_node = -1;
}

/// @brief Move an entity toward a point for one simulation interval.
/// @param entity Entity3D whose world position is updated.
/// @param target Three-component destination in world space.
/// @param speed Movement speed in world units per second.
/// @param arrive Success radius around @p target.
/// @param dt Deterministic simulation interval in seconds.
/// @return BT3D_SUCCESS when already within range, BT3D_RUNNING after movement, or BT3D_FAILURE.
static int game3d_bt_move_toward(
    rt_game3d_entity *entity, const double target[3], double speed, double arrive, double dt) {
    double pos[3];
    if (!game3d_entity_world_position_components(entity, pos))
        return BT3D_FAILURE;
    double to[3] = {target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]};
    double dist = sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (!isfinite(dist))
        return BT3D_FAILURE;
    if (dist <= arrive)
        return BT3D_SUCCESS;
    double step = speed * dt;
    if (step >= dist)
        step = dist;
    rt_game3d_entity_set_position(entity,
                                  pos[0] + to[0] / dist * step,
                                  pos[1] + to[1] / dist * step,
                                  pos[2] + to[2] / dist * step);
    return BT3D_RUNNING;
}

/// @brief Recursively evaluate one behavior-tree node for the current interval.
/// @param world World3D supplying shared simulation services.
/// @param instance Per-entity execution state.
/// @param tree Shared behavior-tree definition.
/// @param node_index Index of the node to evaluate.
/// @param dt Deterministic simulation interval in seconds.
/// @return One of BT3D_FAILURE, BT3D_SUCCESS, or BT3D_RUNNING.
static int game3d_bt_tick_node(rt_game3d_world *world,
                               rt_game3d_bt_instance *instance,
                               rt_game3d_btree *tree,
                               int32_t node_index,
                               double dt);

/// @brief Evaluate a sequence or selector while preserving its running-child cursor.
/// @param world World3D supplying shared simulation services.
/// @param instance Per-entity execution state.
/// @param tree Shared behavior-tree definition.
/// @param node_index Index of the composite node.
/// @param dt Deterministic simulation interval in seconds.
/// @param stop_on Child status that terminates the composite immediately.
/// @return The terminating child status, BT3D_RUNNING, or the composite's inverse terminal status.
static int game3d_bt_tick_composite(rt_game3d_world *world,
                                    rt_game3d_bt_instance *instance,
                                    rt_game3d_btree *tree,
                                    int32_t node_index,
                                    double dt,
                                    int stop_on) {
    rt_game3d_bt_node *node = &tree->nodes[node_index];
    if (node->child_count < 0 || node->child_count > BT3D_MAX_CHILDREN)
        return BT3D_FAILURE;
    int32_t start = instance->running_child[node_index];
    if (start < 0 || start >= node->child_count)
        start = 0;
    for (int32_t c = start; c < node->child_count; ++c) {
        int status = game3d_bt_tick_node(world, instance, tree, node->children[c], dt);
        if (status == BT3D_RUNNING) {
            instance->running_child[node_index] = c;
            return BT3D_RUNNING;
        }
        if (status == stop_on) {
            instance->running_child[node_index] = 0;
            return status;
        }
    }
    instance->running_child[node_index] = 0;
    return stop_on == BT3D_FAILURE ? BT3D_SUCCESS : BT3D_FAILURE;
}

/// @brief Dispatch one behavior node and update the instance-local execution state.
/// @details Composite cursors, wait timers, and custom-leaf handshakes survive across calls;
///   movement leaves update the owner directly using deterministic @p dt.
/// @param world World3D supplying shared simulation services.
/// @param instance Per-entity execution state.
/// @param tree Shared behavior-tree definition.
/// @param node_index Index of the node to evaluate.
/// @param dt Deterministic simulation interval in seconds.
/// @return One of BT3D_FAILURE, BT3D_SUCCESS, or BT3D_RUNNING.
static int game3d_bt_tick_node(rt_game3d_world *world,
                               rt_game3d_bt_instance *instance,
                               rt_game3d_btree *tree,
                               int32_t node_index,
                               double dt) {
    if (!tree || tree->node_count <= 0 || tree->node_count > BT3D_MAX_NODES || node_index < 0 ||
        node_index >= tree->node_count)
        return BT3D_FAILURE;
    rt_game3d_bt_node *node = &tree->nodes[node_index];
    rt_game3d_entity *owner =
        (rt_game3d_entity *)rt_g3d_checked_or_null(instance->entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    if (!owner)
        return BT3D_FAILURE;
    switch (node->type) {
        case BT3D_NODE_SEQUENCE:
            return game3d_bt_tick_composite(world, instance, tree, node_index, dt, BT3D_FAILURE);
        case BT3D_NODE_SELECTOR:
            return game3d_bt_tick_composite(world, instance, tree, node_index, dt, BT3D_SUCCESS);
        case BT3D_NODE_INVERTER: {
            if (node->child_count != 1)
                return BT3D_FAILURE;
            int status = game3d_bt_tick_node(world, instance, tree, node->children[0], dt);
            if (status == BT3D_RUNNING)
                return BT3D_RUNNING;
            return status == BT3D_SUCCESS ? BT3D_FAILURE : BT3D_SUCCESS;
        }
        case BT3D_NODE_CAN_SEE: {
            rt_game3d_perception *sense = (rt_game3d_perception *)rt_g3d_checked_or_null(
                owner->perception, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
            if (!sense)
                return BT3D_FAILURE;
            rt_game3d_entity *target = game3d_bt_instance_retain_target(instance);
            if (!target)
                return BT3D_FAILURE;
            int32_t track_count = game3d_perception_track_count(sense);
            for (int32_t i = 0; i < track_count; ++i) {
                if (sense->tracks[i].seen &&
                    game3d_perception_track_matches(&sense->tracks[i], target)) {
                    game3d_release_ref((void **)&target);
                    return BT3D_SUCCESS;
                }
            }
            game3d_release_ref((void **)&target);
            return BT3D_FAILURE;
        }
        case BT3D_NODE_WAIT: {
            instance->timers[node_index] += dt;
            if (instance->timers[node_index] >= node->p0) {
                instance->timers[node_index] = 0.0;
                return BT3D_SUCCESS;
            }
            return BT3D_RUNNING;
        }
        case BT3D_NODE_MOVE_TO_TARGET: {
            rt_game3d_entity *target = game3d_bt_instance_retain_target(instance);
            if (!target)
                return BT3D_FAILURE;
            double tpos[3];
            int has_position = game3d_entity_world_position_components(target, tpos);
            game3d_release_ref((void **)&target);
            if (!has_position)
                return BT3D_FAILURE;
            return game3d_bt_move_toward(owner, tpos, node->p0, node->p1, dt);
        }
        case BT3D_NODE_MOVE_TO_LAST_KNOWN: {
            rt_game3d_perception *sense = (rt_game3d_perception *)rt_g3d_checked_or_null(
                owner->perception, RT_G3D_GAME3D_PERCEPTION_CLASS_ID);
            if (!sense)
                return BT3D_FAILURE;
            rt_game3d_entity *target = game3d_bt_instance_retain_target(instance);
            if (!target)
                return BT3D_FAILURE;
            int32_t track_count = game3d_perception_track_count(sense);
            for (int32_t i = 0; i < track_count; ++i) {
                if (game3d_perception_track_matches(&sense->tracks[i], target)) {
                    double last_known[3];
                    memcpy(last_known, sense->tracks[i].last_known, sizeof(last_known));
                    game3d_release_ref((void **)&target);
                    return game3d_bt_move_toward(owner, last_known, node->p0, node->p1, dt);
                }
            }
            game3d_release_ref((void **)&target);
            return BT3D_FAILURE;
        }
        case BT3D_NODE_CUSTOM: {
            if (instance->custom_result[node_index] != 0) {
                int status = instance->custom_result[node_index] == 1 ? BT3D_SUCCESS : BT3D_FAILURE;
                instance->custom_result[node_index] = 0;
                return status;
            }
            if (!instance->custom_pending[node_index] && instance->pending_custom_node < 0) {
                instance->custom_pending[node_index] = 1;
                instance->pending_custom_id = node->i0;
                instance->pending_custom_node = node_index;
            }
            return BT3D_RUNNING;
        }
        default:
            return BT3D_FAILURE;
    }
}

/// @brief Per-step AI tick for one entity: perception first, then the tree.
/// @details Ordering lets newly refreshed perception state drive behavior and movement in the same
///   simulation step.
/// @param world World3D containing the entity and simulation services.
/// @param entity Live Entity3D whose attached AI components are advanced.
/// @param dt Deterministic simulation interval in seconds.
void game3d_ai_tick(rt_game3d_world *world, rt_game3d_entity *entity, double dt) {
    if (entity->perception)
        game3d_perception_tick(world, entity, dt);
    if (entity->btree) {
        rt_game3d_bt_instance *instance = (rt_game3d_bt_instance *)rt_g3d_checked_or_null(
            entity->btree, RT_G3D_GAME3D_BTINSTANCE_CLASS_ID);
        rt_game3d_btree *tree = instance ? (rt_game3d_btree *)rt_g3d_checked_or_null(
                                               instance->tree, RT_G3D_GAME3D_BTREE_CLASS_ID)
                                         : NULL;
        if (instance && tree && tree->node_count > 0 && tree->node_count <= BT3D_MAX_NODES &&
            tree->root >= 0 && tree->root < tree->node_count)
            (void)game3d_bt_tick_node(world, instance, tree, tree->root, dt);
    }
}

#else
typedef int rt_game3d_ai_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
