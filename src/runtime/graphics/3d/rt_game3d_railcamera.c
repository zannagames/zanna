//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_railcamera.c
// Purpose: Zanna.Game3D.RailCamera3D — a gameplay-installable spline camera:
//   arclength-constant progress along a Path3D (manual or auto-advance), look
//   targets (entity / point / second path / tangent), piecewise FOV and roll
//   keys, and damped progress jumps. Rides the Path3D arclength evaluator
//   shared with Timeline3D camera-move tracks.
// Key invariants:
//   - Update advances progress; LateUpdate writes the camera post-physics so
//     look-entity targets use final poses (FollowController convention).
//   - Roll composes an explicit up vector into the look-at basis; the camera
//     itself needs no roll state.
// Ownership/Lifetime:
//   - GC-managed; finalizer releases the retained world/path/look references.
// Links: misc/plans/thirdpersonupgrade/10-camera-rails.md, rt_path3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements spline-driven gameplay cameras with arclength motion and keyed lens effects.
/// @details RailCamera3D separates pre-physics progress integration from post-sync
/// camera placement, supports mutually exclusive entity, point, path, or tangent
/// look targets, and evaluates bounded FOV and roll key arrays with linear or
/// smoothstep interpolation.

#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_graphics3d_ids.h"
#include "rt_object.h"
#include "rt_path3d.h"
#include "rt_trap.h"
#include "rt_vec3.h"
#include <math.h>
#include <string.h>

/// @brief Release a retained Vec3 slot, clearing wrong-kind corruption without dropping it.
/// @param[in,out] slot Address of a retained Vec3 slot.
static void game3d_rail_release_vec3(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_is_vec3(*slot)) {
        *slot = NULL;
        return;
    }
    game3d_release_ref(slot);
}

/// @brief Repair, sort, and coalesce one fixed rail-key array.
/// @param[in,out] keys Fixed-capacity array to normalize.
/// @param[in,out] count Raw key count, repaired to the readable range.
/// @param fallback_value Value used for non-finite lanes.
/// @param min_value Inclusive value lower bound.
/// @param max_value Inclusive value upper bound.
static void game3d_rail_repair_keys(rt_game3d_rail_key *keys,
                                    int32_t *count,
                                    double fallback_value,
                                    double min_value,
                                    double max_value) {
    int32_t readable;
    int32_t write = 0;
    if (!keys || !count)
        return;
    readable = *count;
    if (readable >= 0 && readable <= RT_GAME3D_RAIL_MAX_KEYS) {
        int valid = 1;
        for (int32_t i = 0; i < readable; ++i) {
            if (!isfinite(keys[i].t) || keys[i].t < 0.0 || keys[i].t > 1.0 ||
                !isfinite(keys[i].value) || keys[i].value < min_value ||
                keys[i].value > max_value || (i > 0 && keys[i - 1].t >= keys[i].t)) {
                valid = 0;
                break;
            }
        }
        if (valid)
            return;
    }
    if (readable < 0)
        readable = 0;
    if (readable > RT_GAME3D_RAIL_MAX_KEYS)
        readable = RT_GAME3D_RAIL_MAX_KEYS;
    for (int32_t i = 0; i < readable; ++i) {
        rt_game3d_rail_key key = keys[i];
        key.t = game3d_clamp(game3d_finite_or(key.t, 0.0), 0.0, 1.0);
        key.value = game3d_clamp(game3d_finite_or(key.value, fallback_value), min_value, max_value);
        int32_t insert = write;
        while (insert > 0 && keys[insert - 1].t > key.t) {
            keys[insert] = keys[insert - 1];
            --insert;
        }
        keys[insert] = key;
        ++write;
    }
    if (write > 1) {
        int32_t compact = 1;
        for (int32_t read = 1; read < write; ++read) {
            if (keys[read].t == keys[compact - 1].t)
                keys[compact - 1].value = keys[read].value;
            else
                keys[compact++] = keys[read];
        }
        write = compact;
    }
    for (int32_t i = write; i < RT_GAME3D_RAIL_MAX_KEYS; ++i) {
        keys[i].t = 0.0;
        keys[i].value = 0.0;
    }
    *count = write;
}

/// @brief Repair retained slots and every bounded scalar/key invariant on a rail camera.
/// @param rail Rail camera payload to repair; NULL is ignored.
static void game3d_rail_repair_state(rt_game3d_rail_camera *rail) {
    if (!rail)
        return;
    if (rail->world &&
        !rt_obj_is_instance(rail->world, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world)))
        rail->world = NULL;
    if (rail->path && !rt_g3d_has_class(rail->path, RT_G3D_PATH3D_CLASS_ID))
        rail->path = NULL;
    if (rail->look_entity) {
        rt_game3d_entity *entity = rt_obj_is_instance(rail->look_entity,
                                                      RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                      sizeof(rt_game3d_entity))
                                       ? (rt_game3d_entity *)rail->look_entity
                                       : NULL;
        if (!entity)
            rail->look_entity = NULL;
        else if (!game3d_entity_alive_or_record(entity))
            game3d_release_typed_ref(&rail->look_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    }
    if (rail->look_point && !rt_g3d_is_vec3(rail->look_point))
        rail->look_point = NULL;
    if (rail->look_path && !rt_g3d_has_class(rail->look_path, RT_G3D_PATH3D_CLASS_ID))
        rail->look_path = NULL;
    if (rail->look_entity) {
        game3d_rail_release_vec3(&rail->look_point);
        game3d_release_typed_ref(&rail->look_path, RT_G3D_PATH3D_CLASS_ID);
    } else if (rail->look_point) {
        game3d_release_typed_ref(&rail->look_path, RT_G3D_PATH3D_CLASS_ID);
    }
    rail->progress = game3d_clamp(game3d_finite_or(rail->progress, 0.0), 0.0, 1.0);
    rail->smoothed = game3d_clamp(game3d_finite_or(rail->smoothed, rail->progress), 0.0, 1.0);
    rail->speed = game3d_nonnegative_clamped_or(rail->speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
    rail->position_damping =
        game3d_nonnegative_clamped_or(rail->position_damping, 0.0, RT_GAME3D_DAMPING_MAX);
    rail->key_ease = rail->key_ease ? 1 : 0;
    game3d_rail_repair_keys(rail->fov_keys, &rail->fov_key_count, 60.0, 1.0, 179.0);
    game3d_rail_repair_keys(rail->roll_keys, &rail->roll_key_count, 0.0, -720.0, 720.0);
}

/// @brief GC finalizer: release retained references.
/// @param obj Finalized RailCamera3D payload; `NULL` is ignored.
static void game3d_rail_camera_finalize(void *obj) {
    rt_game3d_rail_camera *rail = (rt_game3d_rail_camera *)obj;
    if (!rail)
        return;
    game3d_release_typed_ref(&rail->world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    game3d_release_typed_ref(&rail->path, RT_G3D_PATH3D_CLASS_ID);
    game3d_release_typed_ref(&rail->look_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    game3d_rail_release_vec3(&rail->look_point);
    game3d_release_typed_ref(&rail->look_path, RT_G3D_PATH3D_CLASS_ID);
}

/// @brief Create a rail camera riding @p path in @p world. Defaults: manual
///   progress, damping 0 (snap), linear keys, tangent-facing.
/// @param world_obj Borrowed live World3D handle retained by the controller.
/// @param path Borrowed Path3D handle retained as the camera rail.
/// @return New GC-managed RailCamera3D handle, or `NULL` after validation or allocation failure.
void *rt_game3d_rail_camera_new(void *world_obj, void *path) {
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.RailCamera3D.New: invalid world");
    if (!world)
        return NULL;
    if (!rt_g3d_has_class(path, RT_G3D_PATH3D_CLASS_ID)) {
        rt_trap("Game3D.RailCamera3D.New: path must be Path3D");
        return NULL;
    }
    rt_game3d_rail_camera *rail = (rt_game3d_rail_camera *)rt_obj_new_i64(
        RT_G3D_GAME3D_RAILCAMERA_CLASS_ID, (int64_t)sizeof(*rail));
    if (!rail) {
        rt_trap("Game3D.RailCamera3D.New: allocation failed");
        return NULL;
    }
    memset(rail, 0, sizeof(*rail));
    rt_obj_set_finalizer(rail, game3d_rail_camera_finalize);
    game3d_assign_typed_ref(&rail->world, world, RT_G3D_GAME3D_WORLD_CLASS_ID);
    game3d_assign_typed_ref(&rail->path, path, RT_G3D_PATH3D_CLASS_ID);
    return rail;
}

/// @brief Get the requested arclength-normalized progress [0,1].
/// @param obj Borrowed RailCamera3D handle.
/// @return Requested normalized progress, or zero for an invalid handle.
double rt_game3d_rail_camera_get_progress(void *obj) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.get_progress: invalid rail");
    game3d_rail_repair_state(rail);
    return rail ? rail->progress : 0.0;
}

/// @brief Set the requested progress (clamped [0,1]; damped when Damping > 0).
/// @param obj Borrowed RailCamera3D handle.
/// @param progress Requested normalized progress; non-finite input becomes zero.
void rt_game3d_rail_camera_set_progress(void *obj, double progress) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.set_progress: invalid rail");
    if (rail) {
        game3d_rail_repair_state(rail);
        rail->progress = game3d_clamp(game3d_finite_or(progress, 0.0), 0.0, 1.0);
    }
}

/// @brief Get the auto-advance speed (units/sec along arclength; 0 = manual).
/// @param obj Borrowed RailCamera3D handle.
/// @return Stored non-negative speed in world units per second, or zero when invalid.
double rt_game3d_rail_camera_get_speed(void *obj) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.get_speed: invalid rail");
    game3d_rail_repair_state(rail);
    return rail ? rail->speed : 0.0;
}

/// @brief Set the auto-advance speed.
/// @param obj Borrowed RailCamera3D handle.
/// @param speed Requested non-negative world-units-per-second speed, bounded by the controller
/// limit.
void rt_game3d_rail_camera_set_speed(void *obj, double speed) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.set_speed: invalid rail");
    if (rail)
        rail->speed = game3d_nonnegative_clamped_or(speed, 0.0, RT_GAME3D_CONTROLLER_SPEED_MAX);
}

/// @brief Get the progress damping factor (0 = snap).
/// @param obj Borrowed RailCamera3D handle.
/// @return Stored non-negative exponential damping factor, or zero when invalid.
double rt_game3d_rail_camera_get_position_damping(void *obj) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.get_positionDamping: invalid rail");
    game3d_rail_repair_state(rail);
    return rail ? rail->position_damping : 0.0;
}

/// @brief Set the progress damping factor.
/// @param obj Borrowed RailCamera3D handle.
/// @param damping Requested non-negative exponential damping factor, bounded by the Game3D limit.
void rt_game3d_rail_camera_set_position_damping(void *obj, double damping) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.set_positionDamping: invalid rail");
    if (rail)
        rail->position_damping = game3d_nonnegative_clamped_or(damping, 0.0, RT_GAME3D_DAMPING_MAX);
}

/// @brief Look at an entity's post-physics position (clears other look modes).
/// @param obj Borrowed RailCamera3D handle.
/// @param entity Borrowed Entity3D handle retained as the target, or `NULL` for tangent-facing.
void rt_game3d_rail_camera_set_look_entity(void *obj, void *entity) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.setLookEntity: invalid rail");
    if (!rail)
        return;
    game3d_rail_repair_state(rail);
    rt_game3d_entity *target = NULL;
    if (entity) {
        target = game3d_entity_checked(
            entity, "Game3D.RailCamera3D.setLookEntity: target must be Entity3D");
        if (!target)
            return;
    }
    if (target && !game3d_entity_validate_controller_world(
                      target,
                      (rt_game3d_world *)rail->world,
                      "Game3D.RailCamera3D.setLookEntity: target belongs to another world"))
        return;
    game3d_assign_typed_ref(&rail->look_entity, entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    game3d_rail_release_vec3(&rail->look_point);
    game3d_release_typed_ref(&rail->look_path, RT_G3D_PATH3D_CLASS_ID);
}

/// @brief Look at a fixed point (clears other look modes).
/// @param obj Borrowed RailCamera3D handle.
/// @param point Borrowed Vec3 retained as the target, or `NULL` for tangent-facing.
void rt_game3d_rail_camera_set_look_point(void *obj, void *point) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.setLookPoint: invalid rail");
    if (!rail)
        return;
    game3d_rail_repair_state(rail);
    if (point && !rt_g3d_is_vec3(point)) {
        rt_trap("Game3D.RailCamera3D.setLookPoint: target must be Vec3");
        return;
    }
    game3d_assign_ref(&rail->look_point, point);
    game3d_release_typed_ref(&rail->look_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    game3d_release_typed_ref(&rail->look_path, RT_G3D_PATH3D_CLASS_ID);
}

/// @brief Look along a second path evaluated at the same t (clears other modes).
/// @param obj Borrowed RailCamera3D handle.
/// @param path Borrowed Path3D retained as the target rail, or `NULL` for tangent-facing.
void rt_game3d_rail_camera_set_look_path(void *obj, void *path) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.setLookPath: invalid rail");
    if (!rail)
        return;
    game3d_rail_repair_state(rail);
    if (path && !rt_g3d_has_class(path, RT_G3D_PATH3D_CLASS_ID)) {
        rt_trap("Game3D.RailCamera3D.setLookPath: target must be Path3D");
        return;
    }
    game3d_assign_typed_ref(&rail->look_path, path, RT_G3D_PATH3D_CLASS_ID);
    game3d_release_typed_ref(&rail->look_entity, RT_G3D_GAME3D_ENTITY_CLASS_ID);
    game3d_rail_release_vec3(&rail->look_point);
}

/// @brief Sorted-insert a key into a bounded key array.
/// @param[in,out] keys Fixed-capacity key array receiving the entry.
/// @param[in,out] count Current key count, incremented after insertion.
/// @param t Normalized arclength position, sanitized and clamped to `[0, 1]`.
/// @param value Finite key value; non-finite input becomes zero.
/// @param full_message Diagnostic recorded when the 16-key budget is exhausted.
static void game3d_rail_add_key(
    rt_game3d_rail_key *keys, int32_t *count, double t, double value, const char *full_message) {
    if (!keys || !count)
        return;
    if (*count < 0)
        *count = 0;
    if (*count > RT_GAME3D_RAIL_MAX_KEYS)
        *count = RT_GAME3D_RAIL_MAX_KEYS;
    t = game3d_clamp(game3d_finite_or(t, 0.0), 0.0, 1.0);
    value = game3d_finite_or(value, 0.0);
    for (int32_t existing = 0; existing < *count; ++existing) {
        if (keys[existing].t == t) {
            keys[existing].value = value;
            return;
        }
    }
    if (*count >= RT_GAME3D_RAIL_MAX_KEYS) {
        rt_trap(full_message);
        return;
    }
    int32_t i = *count;
    while (i > 0 && keys[i - 1].t > t) {
        keys[i] = keys[i - 1];
        --i;
    }
    keys[i].t = t;
    keys[i].value = value;
    *count += 1;
}

/// @brief Fluent: add an FOV key at arclength t.
/// @param obj Borrowed RailCamera3D handle.
/// @param t Normalized arclength position, clamped to `[0, 1]`.
/// @param fov Vertical field of view in degrees, clamped to `[1, 179]`.
/// @return Original borrowed rail-camera handle for fluent chaining.
void *rt_game3d_rail_camera_add_fov_key(void *obj, double t, double fov) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.addFovKey: invalid rail");
    if (rail) {
        game3d_rail_repair_state(rail);
        game3d_rail_add_key(rail->fov_keys,
                            &rail->fov_key_count,
                            t,
                            game3d_clamp(game3d_finite_or(fov, 60.0), 1.0, 179.0),
                            "Game3D.RailCamera3D.addFovKey: key limit reached (16)");
    }
    return obj;
}

/// @brief Fluent: add a roll key (degrees about the view axis) at arclength t.
/// @param obj Borrowed RailCamera3D handle.
/// @param t Normalized arclength position, clamped to `[0, 1]`.
/// @param degrees Roll angle bounded to 720 degrees in either direction.
/// @return Original borrowed rail-camera handle for fluent chaining.
void *rt_game3d_rail_camera_add_roll_key(void *obj, double t, double degrees) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.addRollKey: invalid rail");
    if (rail) {
        game3d_rail_repair_state(rail);
        game3d_rail_add_key(rail->roll_keys,
                            &rail->roll_key_count,
                            t,
                            game3d_clamp_abs_or(degrees, 0.0, 720.0),
                            "Game3D.RailCamera3D.addRollKey: key limit reached (16)");
    }
    return obj;
}

/// @brief Get whether keys interpolate with smoothstep instead of linearly.
/// @param obj Borrowed RailCamera3D handle.
/// @return Nonzero for smoothstep interpolation; zero for linear or invalid input.
int8_t rt_game3d_rail_camera_get_key_ease(void *obj) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.get_keyEase: invalid rail");
    game3d_rail_repair_state(rail);
    return rail ? rail->key_ease : 0;
}

/// @brief Choose smoothstep (true) or linear (false) key interpolation.
/// @param obj Borrowed RailCamera3D handle.
/// @param smooth Nonzero to use smoothstep within each key interval.
void rt_game3d_rail_camera_set_key_ease(void *obj, int8_t smooth) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.set_keyEase: invalid rail");
    if (rail)
        rail->key_ease = smooth ? 1 : 0;
}

/// @brief Evaluate a sorted key array at @p t (clamped ends, linear/smoothstep).
/// @param keys Borrowed sorted key array.
/// @param count Number of readable entries.
/// @param t Normalized evaluation position.
/// @param smooth Nonzero to smoothstep the interval fraction.
/// @param[out] out_value Required destination for the evaluated value.
/// @return Nonzero when at least one key was evaluated; zero for an empty array.
static int game3d_rail_eval_keys(
    const rt_game3d_rail_key *keys, int32_t count, double t, int8_t smooth, double *out_value) {
    if (!keys || !out_value || count <= 0)
        return 0;
    if (count > RT_GAME3D_RAIL_MAX_KEYS)
        count = RT_GAME3D_RAIL_MAX_KEYS;
    t = game3d_clamp(game3d_finite_or(t, 0.0), 0.0, 1.0);
    smooth = smooth ? 1 : 0;
    if (t <= keys[0].t) {
        *out_value = game3d_finite_or(keys[0].value, 0.0);
        return 1;
    }
    if (t >= keys[count - 1].t) {
        *out_value = game3d_finite_or(keys[count - 1].value, 0.0);
        return 1;
    }
    for (int32_t i = 0; i + 1 < count; ++i) {
        if (t >= keys[i].t && t <= keys[i + 1].t) {
            double span = keys[i + 1].t - keys[i].t;
            double frac = span > 1e-12 ? (t - keys[i].t) / span : 0.0;
            if (smooth)
                frac = frac * frac * (3.0 - 2.0 * frac);
            *out_value = game3d_finite_or(
                keys[i].value + (keys[i + 1].value - keys[i].value) * frac, keys[i].value);
            return 1;
        }
    }
    *out_value = game3d_finite_or(keys[count - 1].value, 0.0);
    return 1;
}

/// @brief Pre-physics update: auto-advance and damp the progress value.
/// @param obj Borrowed RailCamera3D handle.
/// @param world_obj Borrowed live World3D handle expected to own the controller.
/// @param dt Candidate simulation delta, sanitized and capped before integration.
void rt_game3d_rail_camera_update(void *obj, void *world_obj, double dt) {
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.update: invalid rail");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.RailCamera3D.update: invalid world");
    if (!rail || !world)
        return;
    game3d_rail_repair_state(rail);
    if (!game3d_camera_controller_validate_world(
            rail, world, "Game3D.RailCamera3D.update: controller belongs to another world"))
        return;
    dt = game3d_clamp_controller_dt(dt);
    if (dt <= 0.0)
        return;
    void *path = rt_g3d_checked_or_null(rail->path, RT_G3D_PATH3D_CLASS_ID);
    if (rail->speed > 0.0 && path) {
        double length = rt_path3d_get_length(path);
        if (isfinite(length) && length > 1e-9) {
            rail->progress = game3d_clamp(
                game3d_finite_or(rail->progress + rail->speed * dt / length, 1.0), 0.0, 1.0);
        }
    }
    if (rail->position_damping > 0.0) {
        double alpha = 1.0 - exp(-rail->position_damping * dt);
        rail->smoothed += (rail->progress - rail->smoothed) * game3d_clamp(alpha, 0.0, 1.0);
    } else {
        rail->smoothed = rail->progress;
    }
}

/// @brief Post-sync late update: evaluate the spline + keys and write the camera.
/// @details Resolves look targets after entity transforms are synchronized, rotates
/// the up vector around the view axis for keyed roll, then applies any keyed FOV.
/// @param obj Borrowed RailCamera3D handle.
/// @param world_obj Borrowed live World3D handle expected to own the controller.
/// @param dt Frame delta accepted for the controller interface; placement is progress-driven.
void rt_game3d_rail_camera_late_update(void *obj, void *world_obj, double dt) {
    (void)dt;
    rt_game3d_rail_camera *rail =
        game3d_rail_camera_checked(obj, "Game3D.RailCamera3D.lateUpdate: invalid rail");
    rt_game3d_world *world =
        game3d_world_checked(world_obj, "Game3D.RailCamera3D.lateUpdate: invalid world");
    if (!rail || !world || !world->camera)
        return;
    game3d_rail_repair_state(rail);
    if (!game3d_camera_controller_validate_world(
            rail, world, "Game3D.RailCamera3D.lateUpdate: controller belongs to another world"))
        return;
    void *camera = rt_camera3d_checked_or_stack(world->camera);
    void *path = rt_g3d_checked_or_null(rail->path, RT_G3D_PATH3D_CLASS_ID);
    if (!path || !camera)
        return;
    double t = game3d_clamp(rail->smoothed, 0.0, 1.0);
    double eye[3];
    double tangent[3];
    rt_path3d_eval_spline_raw(path, t, eye, tangent);
    for (int i = 0; i < 3; ++i) {
        eye[i] = game3d_clamp_coord_or(eye[i], 0.0);
        tangent[i] = game3d_clamp_coord_or(tangent[i], i == 2 ? -1.0 : 0.0);
    }

    /* Look target resolution: entity > point > path > tangent. */
    double look[3] = {eye[0] + tangent[0], eye[1] + tangent[1], eye[2] + tangent[2]};
    rt_game3d_entity *look_entity = rt_obj_is_instance(rail->look_entity,
                                                       RT_G3D_GAME3D_ENTITY_CLASS_ID,
                                                       sizeof(rt_game3d_entity))
                                        ? (rt_game3d_entity *)rail->look_entity
                                        : NULL;
    if (look_entity && game3d_entity_alive_or_record(look_entity)) {
        double pos[3];
        if (game3d_entity_world_position_components(look_entity, pos)) {
            look[0] = pos[0];
            look[1] = pos[1];
            look[2] = pos[2];
        }
    } else if (rail->look_point && rt_g3d_is_vec3(rail->look_point)) {
        look[0] = rt_vec3_x(rail->look_point);
        look[1] = rt_vec3_y(rail->look_point);
        look[2] = rt_vec3_z(rail->look_point);
    } else {
        void *look_path = rt_g3d_checked_or_null(rail->look_path, RT_G3D_PATH3D_CLASS_ID);
        if (look_path) {
            double lp[3];
            rt_path3d_eval_spline_raw(look_path, t, lp, NULL);
            look[0] = lp[0];
            look[1] = lp[1];
            look[2] = lp[2];
        }
    }
    for (int i = 0; i < 3; ++i)
        look[i] = game3d_clamp_coord_or(look[i], eye[i] + tangent[i]);

    /* Roll: rotate the base up vector about the view direction. */
    double up[3] = {0.0, 1.0, 0.0};
    double view[3] = {look[0] - eye[0], look[1] - eye[1], look[2] - eye[2]};
    double max_view = fmax(fabs(view[0]), fmax(fabs(view[1]), fabs(view[2])));
    double view_len = 0.0;
    if (max_view > 0.0 && isfinite(max_view)) {
        double sx = view[0] / max_view;
        double sy = view[1] / max_view;
        double sz = view[2] / max_view;
        view_len = max_view * sqrt(sx * sx + sy * sy + sz * sz);
    }
    if (isfinite(view_len) && view_len > 1e-9) {
        view[0] /= view_len;
        view[1] /= view_len;
        view[2] /= view_len;
        if (fabs(view[1]) > 0.99) {
            /* View nearly parallel to +Y: fall back to +X as the base up. */
            up[0] = 1.0;
            up[1] = 0.0;
            up[2] = 0.0;
        }
        double roll_deg = 0.0;
        if (game3d_rail_eval_keys(
                rail->roll_keys, rail->roll_key_count, t, rail->key_ease, &roll_deg) &&
            fabs(roll_deg) > 1e-9) {
            double angle = roll_deg * (RT_GAME3D_PI / 180.0);
            double c = cos(angle);
            double s = sin(angle);
            /* Rodrigues rotation of up about the view axis. */
            double cross[3] = {view[1] * up[2] - view[2] * up[1],
                               view[2] * up[0] - view[0] * up[2],
                               view[0] * up[1] - view[1] * up[0]};
            double dot = view[0] * up[0] + view[1] * up[1] + view[2] * up[2];
            for (int i = 0; i < 3; ++i)
                up[i] = up[i] * c + cross[i] * s + view[i] * dot * (1.0 - c);
        }
    }

    rt_camera3d_look_at_components(camera,
                                   game3d_clamp_coord_or(eye[0], 0.0),
                                   game3d_clamp_coord_or(eye[1], 0.0),
                                   game3d_clamp_coord_or(eye[2], 0.0),
                                   game3d_clamp_coord_or(look[0], 0.0),
                                   game3d_clamp_coord_or(look[1], 0.0),
                                   game3d_clamp_coord_or(look[2], 0.0),
                                   up[0],
                                   up[1],
                                   up[2]);

    double fov = 0.0;
    if (game3d_rail_eval_keys(rail->fov_keys, rail->fov_key_count, t, rail->key_ease, &fov))
        rt_camera3d_set_fov(camera, game3d_clamp(game3d_finite_or(fov, 60.0), 1.0, 179.0));
}
