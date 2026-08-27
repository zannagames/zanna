//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_internal.h
// Purpose: Private shared surface of the Zanna.Game3D layer — tuning constants,
//   the effect-item discriminator, reused Camera3D/Scene/Decal/Particles entry
//   points, and every internal handle-payload struct. Included by rt_game3d.c
//   and its split sibling translation units (rt_game3d_*.c) so they share one
//   definition of the private types without exposing them publicly.
// Key invariants:
//   - Definitions only (no out-of-line code); safe to include from any Game3D TU.
//   - Struct layouts are private ABI — never referenced outside the Game3D TUs.
// Ownership/Lifetime:
//   - Pure declarations; owns no state.
// Links: rt_game3d.h (public API + RT_GAME3D_* constants), rt_gltf.h, rt_input.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines the private data model and cross-translation-unit helpers for Game3D.
/// @details This header centralizes Game3D tuning limits, opaque handle payloads,
/// renderer bridge declarations, checked-cast helpers, and shared subsystem entry
/// points. It is private to the Game3D implementation; callers must use the public
/// API in `rt_game3d.h` rather than depending on these layouts or helper contracts.
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rt_game3d.h"
#include "rt_game3d_diagnostics.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_object.h"
#include "rt_trap.h"

// Default tuning constants applied when callers omit a value or pass a
// non-finite one; chosen for a 60 Hz first-person feel with safe clip planes.
#define RT_GAME3D_DEFAULT_FOV_DEG 60.0    ///< Default vertical camera FOV (degrees).
#define RT_GAME3D_DEFAULT_NEAR 0.1        ///< Default near clip plane (world units).
#define RT_GAME3D_DEFAULT_FAR 1000.0      ///< Default far clip plane (world units).
#define RT_GAME3D_DEFAULT_DT (1.0 / 60.0) ///< Fallback frame delta when timing is invalid.
#define RT_GAME3D_MAX_DT 0.25             ///< Per-frame delta cap (smooths post-stall spikes).
#define RT_GAME3D_MAX_WORKERS 64          ///< Public WorkerCount clamp for internal jobs.
#define RT_GAME3D_DEFAULT_REBASE_THRESHOLD 10000.0 ///< Floating-origin threshold.
#define RT_GAME3D_MIN_REBASE_THRESHOLD 1.0         ///< Avoid constant tiny rebases.
#define RT_GAME3D_DEFAULT_MOVE_SPEED 6.0           ///< Default controller move speed (units/sec).
#define RT_GAME3D_DEFAULT_LOOK_SENSITIVITY 0.01    ///< Default mouse-look degrees per pixel.
#define RT_GAME3D_DEFAULT_JUMP_SPEED 5.5           ///< Default jump launch speed (units/sec).
#define RT_GAME3D_DEFAULT_GRAVITY 20.0         ///< Default downward character gravity magnitude.
#define RT_GAME3D_DEFAULT_FOLLOW_DAMPING 12.0  ///< Default follow-camera smoothing factor.
#define RT_GAME3D_TP_DEFAULT_DISTANCE 4.0      ///< Third-person default boom length.
#define RT_GAME3D_TP_DEFAULT_MIN_DISTANCE 0.75 ///< Third-person boom pull-in floor.
#define RT_GAME3D_TP_DEFAULT_MAX_DISTANCE 8.0  ///< Third-person boom length ceiling.
#define RT_GAME3D_TP_DEFAULT_PIVOT_HEIGHT 1.5  ///< Third-person pivot above entity origin.
#define RT_GAME3D_TP_DEFAULT_SHOULDER_X 0.35   ///< Third-person lateral shoulder offset.
#define RT_GAME3D_TP_DEFAULT_PITCH_MIN (-60.0) ///< Third-person pitch clamp floor (deg).
#define RT_GAME3D_TP_DEFAULT_PITCH_MAX 75.0    ///< Third-person pitch clamp ceiling (deg).
#define RT_GAME3D_TP_DEFAULT_COLLISION_RADIUS 0.25 ///< Third-person boom sweep sphere radius.
#define RT_GAME3D_TP_DEFAULT_AIM_DISTANCE 1.6      ///< Third-person aim-mode boom length.
#define RT_GAME3D_TP_DEFAULT_AIM_FOV 45.0          ///< Third-person aim-mode camera FOV (deg).
#define RT_GAME3D_TP_BOOM_SKIN 0.05                ///< Boom hit back-off epsilon.
#define RT_GAME3D_TP_AIM_BLEND_RATE 6.0            ///< Aim blend speed (fraction per second).
#define RT_GAME3D_TP_FADE_ALPHA 0.35               ///< Occluder fade target alpha.
#define RT_GAME3D_TP_FADE_RATE 8.0                 ///< Occluder fade exponential rate (1/sec).
#define RT_GAME3D_TL_DEFAULT_MAX_DISTANCE 18.0     ///< TargetLock3D acquisition radius.
#define RT_GAME3D_TL_DEFAULT_CONE_DEGREES 65.0     ///< TargetLock3D half-angle cone (deg).
#define RT_GAME3D_TL_DEFAULT_STICKINESS 1.25       ///< TargetLock3D current-target score bonus.
#define RT_GAME3D_TL_DEFAULT_LOS_GRACE 0.5         ///< TargetLock3D LoS-break grace (seconds).
#define RT_GAME3D_DEFAULT_AUDIO_REF_DISTANCE 1.0   ///< Default audio full-volume radius.
#define RT_GAME3D_DEFAULT_AUDIO_MAX_DISTANCE 50.0  ///< Default audio silence radius.
#define RT_GAME3D_AUDIO_DISTANCE_MAX 1000000000.0  ///< Max finite audio attenuation radius.
#define RT_GAME3D_DEFAULT_AUDIO_VOLUME 100         ///< Default master audio volume (0–100).
#define RT_GAME3D_PI 3.14159265358979323846        ///< Pi (avoids relying on non-portable M_PI).
#define RT_GAME3D_ANIM_EVENT_MAX 64                ///< Max animation events buffered per update.
#define RT_GAME3D_MAX_FIXED_STEPS_PER_FRAME 8      ///< Fixed-loop spiral-of-death guard.
#define RT_GAME3D_COORD_ABS_MAX 1000000000000.0    ///< Max finite world coordinate accepted.
#define RT_GAME3D_SCALE_ABS_MAX 1000000.0          ///< Max absolute node/body scale.
#define RT_GAME3D_ANGLE_DEG_ABS_MAX 1000000.0      ///< Max finite Euler/orbit angle in degrees.
#define RT_GAME3D_CONTROLLER_SPEED_MAX 1000000.0   ///< Max controller speed/jump velocity.
#define RT_GAME3D_LOOK_SENSITIVITY_MAX 1000.0      ///< Max mouse-look sensitivity.
#define RT_GAME3D_DAMPING_MAX 1000.0               ///< Max camera damping factor.
#define RT_GAME3D_ANIM_BLEND_TIME_MAX 1000000.0    ///< Max animation transition duration.
#define RT_GAME3D_ANIM_STEP_MAX 1.0                ///< Max single Game3D animator update step.
#define RT_GAME3D_ANIM_SPEED_ABS_MAX 1000000.0     ///< Max animation playback speed multiplier.
#define RT_GAME3D_MAX_ENTITY_NODES 1000000u        ///< Max entities in one world/tree operation.
#define RT_GAME3D_MAX_ENTITY_SET_SLOTS 2097152u    ///< Power-of-two slots for 50%-loaded sets.
#define RT_GAME3D_ENTITY_CHILD_STORAGE_COOKIE                                                      \
    UINT64_C(0x5A4348494C445245) ///< "ZCHILDRE": validates owned raw child storage.
#define RT_GAME3D_WORLD_ENTITY_STORAGE_COOKIE                                                      \
    UINT64_C(0x5A57454E54495459) ///< "ZWENTITY": validates the world entity array.
#define RT_GAME3D_WORLD_ANIMATOR_STORAGE_COOKIE                                                    \
    UINT64_C(0x5A57414E494D4154) ///< "ZWANIMAT": validates animator scratch.
#define RT_GAME3D_WORLD_SEEN_STORAGE_COOKIE                                                        \
    UINT64_C(0x5A575345454E5345) ///< "ZWSEENSE": validates animator dedup scratch.
#define RT_GAME3D_WORLD_JOB_STORAGE_COOKIE                                                         \
    UINT64_C(0x5A574A4F4253544F) ///< "ZWJOBSTO": validates animation jobs.
#define RT_GAME3D_TIMELINE_TRACK_STORAGE_COOKIE                                                    \
    UINT64_C(0x5A544C545241434B) ///< "ZTLTRACK": validates timeline tracks.
#define RT_GAME3D_TP_FADE_STORAGE_COOKIE                                                           \
    UINT64_C(0x5A54504641444553)              ///< "ZTPFADES": validates owned fade storage.
#define RT_GAME3D_EFFECT_STEP_MAX 10.0        ///< Max single EffectRegistry3D update step.
#define RT_GAME3D_EFFECT_LIFETIME_MAX 86400.0 ///< Max effect auto-expire lifetime.
#ifndef RT_GAME3D_MODEL_CACHE_KEY_MAX
#define RT_GAME3D_MODEL_CACHE_KEY_MAX 4096 ///< Max bytes snapshotted for model cache/load paths.
#endif

/// @brief Bound a signed mouse delta before converting it to floating-point controller motion.
/// @param value Candidate backend or snapshot delta.
/// @return Value clamped to the finite Game3D coordinate range.
static inline int64_t game3d_clamp_mouse_delta_i64(int64_t value) {
    const int64_t limit = (int64_t)RT_GAME3D_COORD_ABS_MAX;
    if (value > limit)
        return limit;
    if (value < -limit)
        return -limit;
    return value;
}

/// @brief Compute a geometrically grown private-array capacity without signed or byte overflow.
/// @param current Current non-negative capacity.
/// @param needed Required positive element count.
/// @param initial Positive initial capacity used when @p current is zero.
/// @param element_size Non-zero byte size of one element.
/// @param[out] out_capacity Receives a capacity no smaller than @p needed.
/// @return Non-zero when the requested capacity and allocation byte count are representable.
static inline int game3d_checked_capacity_i32(
    int32_t current, int32_t needed, int32_t initial, size_t element_size, int32_t *out_capacity) {
    int32_t capacity;
    if (!out_capacity || current < 0 || needed <= 0 || initial <= 0 || element_size == 0u)
        return 0;
    if (current >= needed) {
        if ((size_t)current > SIZE_MAX / element_size)
            return 0;
        *out_capacity = current;
        return 1;
    }
    capacity = current > 0 ? current : initial;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity < needed || (size_t)capacity > SIZE_MAX / element_size)
        return 0;
    *out_capacity = capacity;
    return 1;
}

/// @brief Internal effect-item discriminator stored in rt_game3d_effect_item.type.
enum {
    RT_GAME3D_EFFECT_PARTICLES = 1, ///< Item wraps a particle system.
    RT_GAME3D_EFFECT_DECAL = 2,     ///< Item wraps a decal.
};

/// @brief Aim a Camera3D from an eye position toward a target.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param eye_v Borrowed Vec3 containing the world-space eye position.
/// @param target_v Borrowed Vec3 containing the world-space aim point.
/// @param up_v Borrowed Vec3 containing the preferred up direction.
void rt_camera3d_look_at(void *obj, void *eye_v, void *target_v, void *up_v);

/// @brief Initialize the FPS controller angles from a camera's current orientation.
/// @param obj Borrowed heap or stack Camera3D handle.
void rt_camera3d_fps_init(void *obj);

/// @brief Apply one FPS-style look and movement update to a camera.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param yaw_delta Horizontal look delta in degrees.
/// @param pitch_delta Vertical look delta in degrees.
/// @param move_fwd Signed forward-axis input.
/// @param move_right Signed right-axis input.
/// @param move_up Signed world-up input.
/// @param speed Non-negative translation speed in world units per second.
/// @param dt Non-negative elapsed time in seconds.
void rt_camera3d_fps_update(void *obj,
                            double yaw_delta,
                            double pitch_delta,
                            double move_fwd,
                            double move_right,
                            double move_up,
                            double speed,
                            double dt);

/// @brief Position a camera on a sphere around a Vec3 target.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param target_v Borrowed Vec3 containing the orbit center.
/// @param distance Non-negative orbit radius in world units.
/// @param yaw Horizontal orbit angle in degrees.
/// @param pitch Vertical orbit angle in degrees.
void rt_camera3d_orbit(void *obj, void *target_v, double distance, double yaw, double pitch);

/// @brief Allocate a Vec3 containing a camera's logical eye position.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @return New GC-managed Vec3, or `NULL` for invalid input or allocation failure.
void *rt_camera3d_get_position(void *obj);

/// @brief Copy a camera's logical eye position into scalar destinations.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param[out] x Required X-coordinate destination, initialized to zero.
/// @param[out] y Required Y-coordinate destination, initialized to zero.
/// @param[out] z Required Z-coordinate destination, initialized to zero.
/// @return `1` when all destinations receive a valid position; otherwise `0`.
int8_t rt_camera3d_get_position_components(void *obj, double *x, double *y, double *z);

/// @brief Allocate a Vec3 containing a camera's normalized forward direction.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @return New GC-managed normalized Vec3, or `NULL` for invalid input or allocation failure.
void *rt_camera3d_get_forward(void *obj);

/// @brief Allocate a Vec3 containing a camera's normalized right direction.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @return New GC-managed normalized Vec3, or `NULL` for invalid input or allocation failure.
void *rt_camera3d_get_right(void *obj);

/// @brief Aim a camera using unboxed eye, target, and up-vector components.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param eye_x World-space eye X coordinate.
/// @param eye_y World-space eye Y coordinate.
/// @param eye_z World-space eye Z coordinate.
/// @param target_x World-space target X coordinate.
/// @param target_y World-space target Y coordinate.
/// @param target_z World-space target Z coordinate.
/// @param up_x Preferred up-vector X component.
/// @param up_y Preferred up-vector Y component.
/// @param up_z Preferred up-vector Z component.
void rt_camera3d_look_at_components(void *obj,
                                    double eye_x,
                                    double eye_y,
                                    double eye_z,
                                    double target_x,
                                    double target_y,
                                    double target_z,
                                    double up_x,
                                    double up_y,
                                    double up_z);

/// @brief Orbit a camera around an unboxed target position.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param target_x Orbit-center X coordinate.
/// @param target_y Orbit-center Y coordinate.
/// @param target_z Orbit-center Z coordinate.
/// @param distance Non-negative orbit radius in world units.
/// @param yaw Horizontal orbit angle in degrees.
/// @param pitch Vertical orbit angle in degrees.
void rt_camera3d_orbit_components(void *obj,
                                  double target_x,
                                  double target_y,
                                  double target_z,
                                  double distance,
                                  double yaw,
                                  double pitch);

/// @brief Move a camera to a Vec3 world position while preserving its facing direction.
/// @param obj Borrowed heap or stack Camera3D handle.
/// @param pos Borrowed Vec3 containing the new logical eye position.
void rt_camera3d_set_position(void *obj, void *pos);

/// @brief Resolve a SceneNode3D world position into scalar destinations.
/// @param node Borrowed SceneNode3D handle.
/// @param[out] x Required X-coordinate destination.
/// @param[out] y Required Y-coordinate destination.
/// @param[out] z Required Z-coordinate destination.
/// @return `1` on success; `0` without writes for an invalid handle or destination.
int8_t rt_scene_node3d_get_world_position_components(void *node, double *x, double *y, double *z);

/// @brief Shift a decal by the negative floating-origin displacement.
/// @param decal Borrowed Decal3D handle; invalid handles are ignored.
/// @param dx World-origin X displacement.
/// @param dy World-origin Y displacement.
/// @param dz World-origin Z displacement.
void rt_decal3d_rebase_origin(void *decal, double dx, double dy, double dz);

/// @brief Shift a particle emitter and its live particles by a floating-origin displacement.
/// @param particles Borrowed Particles3D handle; invalid handles are ignored.
/// @param dx World-origin X displacement.
/// @param dy World-origin Y displacement.
/// @param dz World-origin Z displacement.
void rt_particles3d_rebase_origin(void *particles, double dx, double dy, double dz);

/// @brief LayerMask payload: a single bitfield of RT_GAME3D_LAYER_* bits.
typedef struct rt_game3d_layermask {
    int64_t bits;
} rt_game3d_layermask;

/// @brief Input3D payload: per-object look sensitivity plus an optional frame snapshot.
typedef struct rt_game3d_input {
    double look_sensitivity;
    int8_t has_snapshot;
    uint8_t key_down[ZANNA_KEY_MAX];
    uint8_t key_pressed[ZANNA_KEY_MAX];
    uint8_t key_released[ZANNA_KEY_MAX];
    uint8_t mouse_down[ZANNA_MOUSE_BUTTON_MAX];
    uint8_t mouse_pressed[ZANNA_MOUSE_BUTTON_MAX];
    uint8_t mouse_released[ZANNA_MOUSE_BUTTON_MAX];
    int64_t mouse_dx;
    int64_t mouse_dy;
    /* Sub-pixel mouse deltas (relative mouse mode); mirror mouse_dx/dy. */
    double mouse_fdx;
    double mouse_fdy;
    /* Absolute window-local cursor position in pixels (ADR 0233); snapshotted
     * alongside the deltas so picking math observes the same frame. */
    int64_t mouse_x;
    int64_t mouse_y;
    double wheel_y;
    /* Gamepad merge: index bound via Input3D.BindPad (-1 = none). Stick axes
     * are snapshotted per update so Move/LookAxis observe a coherent frame. */
    int64_t bound_pad;
    double pad_look_sensitivity;
    double pad_lx;
    double pad_ly;
    double pad_rx;
    double pad_ry;
    int8_t pad_connected;
} rt_game3d_input;

/// @brief Entity3D payload: scene node plus optional mesh/material/body/animator,
///   collision layer/mask, name, owning world, and a dynamic child array.
typedef struct rt_game3d_entity {
    int64_t id;
    void *node;
    void *mesh;
    void *material;
    void *body;
    void *anim;
    void *behavior;  /* retained Behavior3D ticked each simulation step, or NULL */
    void **hitboxes; /* retained Hitbox3D array (combat volumes), or NULL */
    int32_t hitbox_count;
    int32_t hitbox_capacity;
    void *health;       /* retained Health3D component, or NULL */
    void *ragdoll;      /* retained Ragdoll3D built by enableRagdoll, or NULL */
    void *lipsync;      /* retained LipSync3D component, or NULL */
    void *footsteps;    /* retained Footsteps3D component, or NULL */
    void *interactable; /* retained Interactable3D component, or NULL */
    void *interactor;   /* retained Interactor3D component, or NULL */
    void *perception;   /* retained Perception3D component, or NULL */
    void *btree;        /* retained BehaviorTreeInstance3D, or NULL */
    int64_t layer;
    int64_t collision_mask_bits;
    rt_string name;
    void *world;
    struct rt_game3d_entity *parent;
    struct rt_game3d_entity **children;
    int32_t child_count;
    int32_t child_capacity;
    int32_t registry_index; /* slot in owning world's dense entity array, -1 when unspawned */
    int8_t group;
    int8_t alive;
    int8_t spawned;
    int8_t destroyed;
    /* Fixed-step render interpolation (world->render_interpolation): node pose captured
     * before the latest fixed simulation step, plus scratch to restore the authoritative
     * sim pose after an interpolated render. */
    double interp_prev_position[3];
    double interp_prev_rotation[4];
    double interp_saved_position[3];
    double interp_saved_rotation[4];
    int8_t interp_has_prev;
    int8_t interp_pose_blended;
    rt_string persistent_key; ///< Retained persistence key, or NULL (plan 17).
    int64_t state_tag;        ///< Free-form persisted state tag.
    /// Last world sweep stamp that ticked this entity (despawn-safe sweeps).
    /// Appended at the end: test fixtures mirror prefixes of this layout.
    uint32_t sim_tick_stamp;
    /// Trusted capacity paired with the raw `children` allocation.
    int32_t child_storage_capacity;
    /// Address/capacity ownership marker; appended to preserve fixture prefixes.
    uint64_t child_storage_cookie;
} rt_game3d_entity;

/// @brief Derive the integrity marker for an entity's raw child allocation.
/// @param children Allocation address, or NULL.
/// @param capacity Number of pointer slots owned by the allocation.
/// @return Marker binding the allocation address and capacity to this payload.
static inline uint64_t game3d_entity_child_storage_cookie_value(const void *children,
                                                                int32_t capacity) {
    uint64_t address = (uint64_t)(uintptr_t)children;
    uint64_t size = (uint64_t)(uint32_t)capacity;
    return RT_GAME3D_ENTITY_CHILD_STORAGE_COOKIE ^ address ^ (size * UINT64_C(0x9E3779B185EBCA87));
}

/// @brief Bind a world-owned raw allocation to its address, capacity, and slot discriminator.
/// @param storage Allocation address, or NULL.
/// @param capacity Number of elements owned by the allocation.
/// @param discriminator Nonzero per-slot cookie constant.
/// @return Integrity marker for the allocation tuple.
static inline uint64_t game3d_world_storage_cookie_value(const void *storage,
                                                         size_t capacity,
                                                         uint64_t discriminator) {
    uint64_t address = (uint64_t)(uintptr_t)storage;
    uint64_t size = (uint64_t)capacity;
    return discriminator ^ address ^ (size * UINT64_C(0x9E3779B185EBCA87));
}

/// @brief Check one world-owned raw allocation against its authoritative storage tuple.
/// @param storage Allocation address.
/// @param capacity Authoritative element capacity.
/// @param cookie Stored integrity marker.
/// @param discriminator Per-slot cookie constant.
/// @return Nonzero only for a nonempty matching allocation tuple.
static inline int game3d_world_storage_is_valid(const void *storage,
                                                size_t capacity,
                                                uint64_t cookie,
                                                uint64_t discriminator) {
    return storage && capacity > 0 &&
           cookie == game3d_world_storage_cookie_value(storage, capacity, discriminator);
}

/// @brief Return the entity's SceneNode3D slot only when it still has the expected class.
/// @param entity Borrowed entity payload, or `NULL`.
/// @return Borrowed SceneNode3D handle, or `NULL` when absent, stale, or type-mismatched.
static inline void *game3d_entity_node_ref(const rt_game3d_entity *entity) {
    return entity ? rt_g3d_checked_or_null(entity->node, RT_G3D_SCENENODE3D_CLASS_ID) : NULL;
}

/// @brief Return the entity's Mesh3D slot only when it still has the expected class.
/// @param entity Borrowed entity payload, or `NULL`.
/// @return Borrowed Mesh3D handle, or `NULL` when absent, stale, or type-mismatched.
static inline void *game3d_entity_mesh_ref(const rt_game3d_entity *entity) {
    return entity ? rt_g3d_checked_or_null(entity->mesh, RT_G3D_MESH3D_CLASS_ID) : NULL;
}

/// @brief Return the entity's Material3D slot only when it still has the expected class.
/// @param entity Borrowed entity payload, or `NULL`.
/// @return Borrowed Material3D handle, or `NULL` when absent, stale, or type-mismatched.
static inline void *game3d_entity_material_ref(const rt_game3d_entity *entity) {
    return entity ? rt_g3d_checked_or_null(entity->material, RT_G3D_MATERIAL3D_CLASS_ID) : NULL;
}

/// @brief Return the entity's Physics3DBody slot only when it still has the expected class.
/// @param entity Borrowed entity payload, or `NULL`.
/// @return Borrowed Physics3DBody handle, or `NULL` when absent, stale, or type-mismatched.
static inline void *game3d_entity_body_ref(const rt_game3d_entity *entity) {
    return entity ? rt_g3d_checked_or_null(entity->body, RT_G3D_BODY3D_CLASS_ID) : NULL;
}

/// @brief Return the entity's Animator3D slot only when it still has the expected class.
/// @param entity Borrowed entity payload, or `NULL`.
/// @return Borrowed Animator3D handle, or `NULL` when absent, stale, or type-mismatched.
static inline void *game3d_entity_anim_ref(const rt_game3d_entity *entity) {
    return entity ? rt_g3d_checked_or_null(entity->anim, RT_G3D_GAME3D_ANIMATOR3D_CLASS_ID) : NULL;
}

/// @brief Safe dense prefix of child entity slots that may be read directly.
/// @param entity Borrowed entity payload, or `NULL`.
/// @return Valid dense child count recovered from trusted storage, or zero for invalid storage.
static inline int32_t game3d_entity_child_count(const rt_game3d_entity *entity) {
    if (!entity || !entity->children ||
        entity->child_storage_cookie != game3d_entity_child_storage_cookie_value(
                                            entity->children, entity->child_storage_capacity) ||
        entity->child_storage_capacity <= 0)
        return 0;
    for (int32_t i = 0; i < entity->child_storage_capacity; ++i) {
        if (!entity->children[i])
            return i;
        if (!rt_obj_is_instance(
                entity->children[i], RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity)))
            return i;
    }
    return entity->child_storage_capacity;
}

/// @brief Sound3D payload: listener, optional followed camera, a dynamic source
///   list, distance-attenuation radii, master volume, and follow-camera flag.
typedef struct rt_game3d_audio {
    void *listener;
    void *camera;
    void **sources;
    int32_t source_count;
    int32_t source_capacity;
    double ref_distance;
    double max_distance;
    int64_t volume;
    int8_t listener_follow_camera;
    /* Audio immersion (plan 24): reverb zones, occlusion raycasts, ambient beds. */
    void **reverb_zones;          ///< Retained ReverbZone3D handles.
    int32_t reverb_zone_count;    ///< Number of registered zones.
    int32_t reverb_zone_capacity; ///< Zone array capacity.
    int64_t reverb_group;         ///< Lazily-registered "g3d_reverb" group (-1 unset).
    int64_t reverb_fx;            ///< Reverb insert id on the group (-1 unset).
    double reverb_blend;          ///< Zone parameter blend time in seconds.
    double reverb_room;           ///< Current eased room size.
    double reverb_damp;           ///< Current eased damping.
    double reverb_wet;            ///< Current eased wet mix.
    int8_t reverb_routing;        ///< Route new positional voices to the reverb group.
    int8_t occlusion_enabled;     ///< Listener->source raycast occlusion on/off.
    int64_t occlusion_mask;       ///< Raycast layer mask for occlusion probes.
    double occlusion_amount;      ///< Occlusion applied to a blocked source (0..1).
    int32_t occlusion_budget;     ///< Max raycasts per world step.
    int32_t occlusion_cursor;     ///< Round-robin cursor over tracked sources.
    int64_t dialogue_group;       ///< Lazily-registered "g3d_dialogue" group (-1 unset).
    void *ambient_bed;            ///< Retained AmbientBed3D (NULL when unused).
} rt_game3d_audio;

/// @brief One registered effect: its kind (RT_GAME3D_EFFECT_*), the wrapped
///   object, an auto-expire lifetime, and the accumulated age in seconds.
typedef struct rt_game3d_effect_item {
    int64_t type;
    void *object;
    double lifetime;
    double age;
} rt_game3d_effect_item;

/// @brief EffectRegistry3D payload: the world's post-FX stack plus a dynamic
///   array of live effect items advanced and retired each frame.
typedef struct rt_game3d_effects {
    void *postfx;
    rt_game3d_effect_item *items;
    int32_t count;
    int32_t capacity;
} rt_game3d_effects;

/// @brief EnvHandle payload: the target world plus the optional terrain and
///   water entities a fluent environment builder has attached.
typedef struct rt_game3d_env_handle {
    void *world;
    void *terrain_entity;
    void *water_entity;
    double terrain_size;
    int8_t has_terrain_size;
} rt_game3d_env_handle;

/// @brief BodyDef payload: shape kind and dimensions, mass/material properties,
///   collision layer/mask, sync mode, and dynamic/kinematic/trigger/CCD flags.
typedef struct rt_game3d_body_def {
    int64_t shape;
    double half_extents[3];
    double radius;
    double height;
    double mass;
    double friction;
    double restitution;
    int64_t layer;
    int64_t mask_bits;
    int64_t sync_mode;
    int8_t has_layer;
    int8_t has_mask;
    int8_t is_static;
    int8_t is_kinematic;
    int8_t is_trigger;
    int8_t use_ccd;
} rt_game3d_body_def;

/// @brief Collision3DEvent payload: the phase, the two participating entities,
///   and the underlying low-level physics collision event.
typedef struct rt_game3d_collision_event {
    int64_t phase;
    void *a;
    void *b;
    void *raw;
} rt_game3d_collision_event;

/// @brief Animator3D payload: optional skeletal controller, optional node animator,
///   plus names of skeletal events fired during the most recent update.
typedef struct rt_game3d_animator {
    void *controller;
    void *node_animator;
    rt_string events[RT_GAME3D_ANIM_EVENT_MAX];
    int32_t event_count;
} rt_game3d_animator;

/// @brief ModelTemplate payload: the source path, whether it is a packed asset,
///   and the loaded model that fresh instances are cloned from.
typedef struct rt_game3d_model_template {
    rt_string path;
    int8_t asset_path;
    void *model;
} rt_game3d_model_template;

/// @brief AssetHandle3D payload: request state plus either an entity result or
///   a reusable ModelTemplate result. Deferred handles schedule worker loading
///   on first observation and publish terminal results through the main-thread
///   commit queue.
typedef struct rt_game3d_asset_handle {
    int8_t ready;
    int8_t cancelled;
    int8_t deferred;
    int8_t async_started;
    int8_t asset_path;
    int8_t template_request;
    double progress;
    rt_string error;
    rt_string path;
    void *entity;
    void *model_template;
} rt_game3d_asset_handle;

/// @brief Async asset-load worker job: the target AssetHandle3D, snapshotted request
///   metadata safe for worker-thread reads, any preloaded glTF bundle or FBX root bytes,
///   the cache generation it was scheduled against, upload-byte progress counters, and a
///   fixed-size error buffer filled on worker-thread failure.
typedef struct rt_game3d_asset_async_job {
    rt_game3d_asset_handle *handle;
    char path[RT_GAME3D_MODEL_CACHE_KEY_MAX];
    int8_t asset_path;
    int8_t template_request;
    rt_gltf_preload_bundle *preloaded_gltf;
    uint8_t *preloaded_fbx_data;
    size_t preloaded_fbx_size;
    /* Plan 59 / ADR 0257 companion: VSCN/scene3d root bytes read off-thread
     * so the main-thread commit parses from memory instead of doing the
     * (potentially hundreds of MB) file IO itself. */
    uint8_t *preloaded_vscn_data;
    size_t preloaded_vscn_size;
    uint64_t cache_generation;
    uint64_t upload_total_bytes;
    uint64_t upload_prepared_bytes;
    char error[256];
} rt_game3d_asset_async_job;

/// @brief One streaming scene cell parsed from the cells manifest: spatial center/
///   radius, material/nav/layer/collision metadata, an optional malloc-owned binary
///   sidecar payload, and the loaded scene/entity plus residency bookkeeping.
typedef struct rt_game3d_stream_cell {
    rt_string name;
    rt_string path;
    double center[3];
    double radius;
    int64_t resident_bytes;
    int64_t measured_resident_bytes;
    rt_string material;
    rt_string nav_area;
    rt_string sidecar_path;
    int64_t layer;
    int64_t collision_mask;
    double traversal_cost;
    int8_t has_layer;
    int8_t has_collision_mask;
    int8_t collision_enabled;
    void *scene;
    void *entity;
    int8_t resident;
    void *sidecar_data;        /* loaded binary sidecar payload (malloc-owned), or NULL */
    int64_t sidecar_bytes;     /* size in bytes of the loaded binary sidecar payload */
    int32_t reload_cooldown;   /* recompute passes to wait before reloading after a
                                  budget eviction (prevents load/unload thrash) */
    int8_t staging;            /* async: a worker staging job is in flight */
    int8_t staged;             /* async: worker payload landed, awaiting main commit */
    int8_t staged_error;       /* async: staging failed (missing/corrupt payload) */
    int8_t prefetched;         /* staged/staging due to velocity prefetch only */
    char *staged_text;         /* staged .vscn text (malloc-owned), or NULL */
    size_t staged_text_len;    /* byte length of staged_text */
    uint8_t *staged_sidecar;   /* staged sidecar bytes (malloc-owned), or NULL */
    size_t staged_sidecar_len; /* byte length of staged_sidecar */
    /* --- HLOD proxy ring (cell-level merged low-poly stand-in) --- */
    rt_string proxy_path;         /* optional manifest "proxy" .vscn path, or NULL */
    int64_t proxy_bytes;          /* manifest "proxyBytes" estimate */
    int64_t measured_proxy_bytes; /* measured proxy residency after load */
    void *proxy_scene;            /* loaded proxy Scene3D while ProxyResident */
    void *proxy_entity;           /* spawned proxy entity subtree while ProxyResident */
    int8_t proxy_resident;        /* proxy subtree currently attached */
    int8_t proxy_staging;         /* async proxy staging job in flight */
    int8_t proxy_staged;          /* staged proxy text awaiting commit */
    char *staged_proxy_text;      /* staged proxy .vscn text (malloc-owned) */
    size_t staged_proxy_text_len;
} rt_game3d_stream_cell;

/// @brief Scratch entry used to sort manifest-backed streaming loads nearest-first.
/// @details WorldStream3D owns reusable arrays of these entries so steady-state
///   streaming updates do not allocate every frame. The index is stable within the
///   parsed manifest array and distance_sq is the sanitized squared focus distance.
typedef struct rt_game3d_stream_load_candidate {
    int32_t index;
    double distance_sq;
} rt_game3d_stream_load_candidate;

/// @brief One streaming terrain tile parsed from the terrain manifest: spatial
///   center/scale/radius and heightmap, material/nav/layer/collision metadata, an
///   optional malloc-owned binary sidecar payload, and the loaded terrain plus
///   collider/nav entities and residency bookkeeping.
typedef struct rt_game3d_stream_terrain_tile {
    rt_string name;
    rt_string path;
    rt_string heightmap_path;
    double center[3];
    double scale[3];
    double radius;
    int64_t width;
    int64_t depth;
    int64_t resident_bytes;
    rt_string material;
    rt_string nav_area;
    rt_string sidecar_path;
    int64_t layer;
    int64_t collision_mask;
    double traversal_cost;
    int8_t has_layer;
    int8_t has_collision_mask;
    int8_t collision_enabled;
    void *terrain;
    void *collider_entity;
    void *nav_entity;
    int8_t resident;
    void *sidecar_data;        /* loaded binary sidecar payload (malloc-owned), or NULL */
    int64_t sidecar_bytes;     /* size in bytes of the loaded binary sidecar payload */
    int32_t reload_cooldown;   /* recompute passes to wait before reloading after a
                                  budget eviction (prevents load/unload thrash) */
    int8_t staging;            /* async: a worker staging job is in flight */
    int8_t staged;             /* async: worker payload landed, awaiting main commit */
    int8_t staged_error;       /* async: staging failed (missing/corrupt payload) */
    int8_t prefetched;         /* staged/staging due to velocity prefetch only */
    double *staged_heights;    /* staged POD height grid (malloc-owned), or NULL */
    int64_t staged_hm_width;   /* source width of staged_heights */
    int64_t staged_hm_depth;   /* source depth of staged_heights */
    uint8_t *staged_sidecar;   /* staged sidecar bytes (malloc-owned), or NULL */
    size_t staged_sidecar_len; /* byte length of staged_sidecar */
    double (*holes)[4];        /* manifest-authored hole rects (tile-local units) */
    int32_t manifest_hole_count;
} rt_game3d_stream_terrain_tile;

/// @brief WorldStream3D payload: streaming focus/radii, mounted manifest paths,
///   parsed scene-cell manifests, and deterministic resident telemetry.
#define RT_GAME3D_MAX_HITCHES 256
#define RT_GAME3D_HITCH_SOURCE_STREAM_COMMIT 0
#define RT_GAME3D_HITCH_SOURCE_FRAME_TOTAL 3

/// @brief One recorded hitch: frame index, source constant, and wall ms.
typedef struct rt_game3d_hitch_entry {
    int64_t frame;  ///< World frame counter when the hitch was recorded.
    int64_t source; ///< RT_GAME3D_HITCH_SOURCE_* constant.
    double ms;      ///< Measured milliseconds.
} rt_game3d_hitch_entry;

/// @brief One persisted-entity delta record (plan 17): keyed pose + liveness.
typedef struct rt_game3d_persist_record {
    rt_string key;          ///< Retained game-stable persistence key.
    int8_t alive;           ///< 0 once the keyed entity was despawned/killed.
    int8_t applied_pending; ///< Loaded from a snapshot, not yet applied to an entity.
    double position[3];     ///< Last captured world position.
    double rotation[4];     ///< Last captured world rotation quaternion (xyzw).
    double scale[3];        ///< Reserved (identity in v1).
    int64_t state_tag;      ///< Free-form game state tag.
} rt_game3d_persist_record;

/// @brief One per-cell persisted flag (door-opened / chest-looted style).
typedef struct rt_game3d_cell_flag {
    rt_string cell; ///< Retained cell name (manifest key).
    rt_string key;  ///< Retained flag key.
    int64_t value;  ///< Flag value.
} rt_game3d_cell_flag;

#define RT_GAME3D_STREAM_MAX_LOADED_EVENTS 32

typedef struct rt_game3d_world_stream {
    void *world;
    double center[3];
    double load_radius;
    double unload_radius;
    int64_t residency_budget_bytes;
    int64_t resident_cell_count;
    int64_t resident_terrain_tile_count;
    int64_t pending_request_count;
    int64_t resident_bytes;
    rt_string terrain_manifest;
    rt_string cells_manifest;
    rt_game3d_stream_cell *cells;
    rt_game3d_stream_terrain_tile *terrain_tiles;
    int32_t cell_count;
    int32_t cell_capacity;
    int32_t terrain_tile_count;
    int32_t terrain_tile_capacity;
    rt_game3d_stream_load_candidate *cell_candidates;
    int32_t cell_candidate_capacity;
    rt_game3d_stream_load_candidate *terrain_candidates;
    int32_t terrain_candidate_capacity;
    int8_t cells_manifest_loaded;
    int8_t terrain_manifest_loaded;
    int8_t retains_world;
    /* --- worker-backed streaming (plan: async cell/tile staging) --- */
    int8_t async_streaming;          /* 1 = worker staging + budgeted main commits (default) */
    uint64_t cell_generation;        /* bumped on cell mount/clear so late results drop */
    uint64_t terrain_generation;     /* bumped on terrain mount/clear so late results drop */
    int64_t commit_budget_bytes;     /* staged bytes committed per update; -1 = unlimited,
                                        0 = hold commits pending */
    double prefetch_lookahead;       /* seconds of center velocity to prefetch along */
    double prev_center[3];           /* center at the previous update (velocity estimate) */
    double velocity[3];              /* smoothed center velocity (units/sec) */
    int8_t has_prev_center;          /* prev_center is valid */
    double stream_stall_ms;          /* worst single commit-slice wall ms since mount */
    int64_t prefetched_cell_count;   /* cells staged/staging from prefetch only */
    double proxy_radius;             /* HLOD proxy ring radius; <=0 = auto (4x load) */
    int64_t proxy_resident_count;    /* cells currently holding only their proxy */
    int64_t proxy_resident_bytes;    /* measured bytes of resident proxy subtrees */
    rt_game3d_cell_flag *cell_flags; /* persisted per-cell flags (plan 17) */
    int32_t cell_flag_count;         /* number of flags */
    int32_t cell_flag_capacity;      /* flag array capacity */
    rt_string loaded_events[RT_GAME3D_STREAM_MAX_LOADED_EVENTS]; /* just-loaded cell names */
    int32_t loaded_event_count;                                  /* buffered loaded-cell events */
} rt_game3d_world_stream;

/// @brief One entry in the process-wide model cache, keyed by path + asset flag
///   and mapping to its shared ModelTemplate.
typedef struct rt_game3d_model_cache_entry {
    rt_string path;
    int8_t asset_path;
    int8_t loading;
    void *model_template;
    uint64_t resident_bytes;
    uint64_t last_used;
    double residency_priority;
    double residency_distance;
} rt_game3d_model_cache_entry;

/// @brief CharacterController3D payload: owning world, driven entity, underlying
///   character object, movement tuning, integrated vertical velocity / eye height,
///   and the crouch/stand capsule heights.
typedef struct rt_game3d_character_controller {
    void *world;
    void *entity;
    void *character;
    double speed;
    double jump_speed;
    double gravity;
    double vertical_velocity;
    double eye_height;
    double stand_height;   ///< Capsule height restored by SetCrouching(false).
    double crouch_height;  ///< Capsule height applied by SetCrouching(true).
    double capsule_radius; ///< Capsule radius captured at creation (probe sugar).
    int8_t crouching;      ///< Current requested crouch state.
} rt_game3d_character_controller;

/// @brief FirstPersonController payload: owning world, the character controller it
///   drives, move speed, look sensitivity, and whether the cursor is captured.
typedef struct rt_game3d_first_person_controller {
    void *world;
    void *character_controller;
    double speed;
    double look_sensitivity;
    int8_t capture_mouse;
} rt_game3d_first_person_controller;

/// @brief FreeFlyController payload: owning world, fly speed, look sensitivity,
///   and whether the cursor is captured.
typedef struct rt_game3d_free_fly_controller {
    void *world;
    double speed;
    double look_sensitivity;
    int8_t capture_mouse;
} rt_game3d_free_fly_controller;

/// @brief OrbitController payload: owning world, orbit target, clamped distance
///   range, current yaw/pitch, and orbit/zoom input sensitivities.
typedef struct rt_game3d_orbit_controller {
    void *world;
    void *target;
    double distance;
    double min_distance;
    double max_distance;
    double yaw;
    double pitch;
    double orbit_sensitivity;
    double zoom_sensitivity;
} rt_game3d_orbit_controller;

/// @brief FollowController payload: owning world, followed entity, position
///   offset relative to the target, and the smoothing damping factor.
typedef struct rt_game3d_follow_controller {
    void *world;
    void *target_entity;
    void *offset;
    double damping;
} rt_game3d_follow_controller;

/// @brief One occluder-fade bookkeeping entry for the third-person controller:
///   the scene node whose material was swapped, the original material handle to
///   restore, the faded instance clone, and the current animated alpha.
typedef struct rt_game3d_tp_fade_entry {
    void *node;              ///< Retained scene node whose material is swapped.
    void *original_material; ///< Retained original material to restore.
    void *fade_material;     ///< Retained Material3D.MakeInstance clone being faded.
    double alpha;            ///< Current animated alpha on the clone.
    double original_alpha;   ///< Alpha of the original material at capture time.
    int8_t occluding;        ///< Marked each late update while still occluding.
} rt_game3d_tp_fade_entry;

/// @brief ThirdPersonController payload: owning world, orbited target entity,
///   optional character-drive slot, spring-arm orbit state (yaw/pitch/distance),
///   shoulder/pivot framing, boom collision tuning, aim-mode blend state, and
///   occluder-fade bookkeeping.
typedef struct rt_game3d_thirdperson_controller {
    void *world;               ///< Weak-style retained world back-ref (controller-slot pattern).
    void *target;              ///< Entity3D the camera orbits (usually the player).
    void *character;           ///< Optional CharacterController3D drive slot.
    void *lock;                ///< Optional TargetLock3D framing source (plan 02).
    double yaw;                ///< Camera orbit yaw in degrees.
    double pitch;              ///< Camera orbit pitch in degrees.
    double distance;           ///< Desired boom length.
    double min_distance;       ///< Boom pull-in floor.
    double max_distance;       ///< Boom length ceiling.
    double shoulder_offset[3]; ///< Local-space offset from the target pivot.
    double pivot_height;       ///< Pivot height above the entity origin.
    double pitch_min;          ///< Pitch clamp floor in degrees.
    double pitch_max;          ///< Pitch clamp ceiling in degrees.
    double damping;            ///< Exponential smoothing rate for boom release.
    double collision_radius;   ///< Boom sweep sphere radius.
    int64_t collision_mask;    ///< Layers the boom collides with.
    int8_t occlusion_fade;     ///< Opt-in occluder fading toggle.
    int8_t aiming;             ///< Aim-mode request flag.
    double aim_blend;          ///< 0..1 current aim interpolation.
    double aim_distance;       ///< Boom length while aiming.
    double aim_fov;            ///< Camera FOV while aiming (degrees).
    double aim_shoulder_offset[3];  ///< Shoulder offset while aiming.
    double base_fov;                ///< Camera FOV captured when aim blend engages.
    int8_t base_fov_valid;          ///< True while base_fov holds a captured value.
    double current_distance;        ///< Smoothed post-collision boom length.
    rt_game3d_tp_fade_entry *fades; ///< Occluder-fade bookkeeping array.
    int32_t fade_count;             ///< Live fade entries.
    int32_t fade_capacity;          ///< Allocated fade entries.
    int32_t fade_storage_capacity;  ///< Trusted capacity paired with `fades`.
    uint64_t fade_storage_cookie;   ///< Address/capacity ownership marker.
} rt_game3d_thirdperson_controller;

/// @brief Derive the integrity marker for a third-person fade allocation.
/// @param fades Allocation address, or NULL.
/// @param capacity Number of fade entries owned by the allocation.
/// @return Marker binding the allocation address and capacity to this payload.
static inline uint64_t game3d_thirdperson_fade_storage_cookie_value(const void *fades,
                                                                    int32_t capacity) {
    uint64_t address = (uint64_t)(uintptr_t)fades;
    uint64_t size = (uint64_t)(uint32_t)capacity;
    return RT_GAME3D_TP_FADE_STORAGE_COOKIE ^ address ^ (size * UINT64_C(0xA24BAED4963EE407));
}

#define RT_GAME3D_TL_MAX_MARKERS_PER_STEP 64 ///< Marker events buffered per tick.
#define RT_GAME3D_TL_TEXT_MAX 256            ///< Subtitle/name text capacity per track.
#define RT_GAME3D_TL_MAX_TRACKS 65536        ///< Resource ceiling for one timeline.

/// @brief Timeline3D track kinds.
enum {
    RT_GAME3D_TL_CAMERA_CUT = 0,
    RT_GAME3D_TL_CAMERA_MOVE = 1,
    RT_GAME3D_TL_FOV_RAMP = 2,
    RT_GAME3D_TL_ANIM = 3,
    RT_GAME3D_TL_AUDIO = 4,
    RT_GAME3D_TL_SUBTITLE = 5,
    RT_GAME3D_TL_LETTERBOX = 6,
    RT_GAME3D_TL_FADE = 7,
    RT_GAME3D_TL_MARKER = 8,
};

/// @brief One Timeline3D track record (flat union of per-kind fields).
typedef struct rt_game3d_tl_track {
    int8_t type;                        ///< RT_GAME3D_TL_*.
    double t0;                          ///< Start time (fire time for point tracks).
    double t1;                          ///< End time (== t0 for point tracks).
    int8_t fired;                       ///< Fire-once latch, reset by play()/stop().
    int8_t ease;                        ///< 0 linear, 1 smoothstep, 2 ease-in, 3 ease-out.
    double vec_a[3];                    ///< Cut position / audio position.
    double vec_b[3];                    ///< Cut look-at point.
    double scalar_a;                    ///< Cut/ramp fov0 / letterbox amount / fade a0 / crossfade.
    double scalar_b;                    ///< Ramp fov1 / fade a1.
    void *obj_a;                        ///< Retained Path3D (move) or audio clip.
    void *obj_b;                        ///< Retained look target (Vec3 | Entity3D | Path3D).
    int8_t positional;                  ///< Audio: play at vec_a instead of 2D.
    int64_t marker_id;                  ///< Marker payload.
    char text_a[RT_GAME3D_TL_TEXT_MAX]; ///< Anim entity name / subtitle text.
    char text_b[RT_GAME3D_TL_TEXT_MAX]; ///< Anim state name.
} rt_game3d_tl_track;

/// @brief Timeline3D payload: track list + playhead + polled marker buffer.
typedef struct rt_game3d_timeline {
    void *world; ///< Retained world while installed as active timeline.
    rt_game3d_tl_track *tracks;
    int32_t track_count;
    int32_t track_capacity;
    double time;
    double duration;
    int8_t playing;
    int8_t finished;
    int8_t just_finished;
    int8_t skippable;
    int8_t sorted;
    int8_t has_camera_tracks; ///< Suspends the installed camera controller.
    int64_t fired_markers[RT_GAME3D_TL_MAX_MARKERS_PER_STEP];
    int32_t fired_marker_count;
    char active_subtitle[RT_GAME3D_TL_TEXT_MAX];
    double letterbox_amount; ///< Current letterbox fraction (overlay pass).
    double fade_alpha;       ///< Current full-screen fade alpha (overlay pass).
    /// Immutable allocation identity and initialized-slot count. Appended so
    /// existing white-box test prefixes keep their offsets.
    rt_game3d_tl_track *owned_tracks;
    int32_t track_storage_count;
    int32_t track_storage_capacity;
    uint64_t track_storage_cookie;
} rt_game3d_timeline;

#define RT_GAME3D_DLG_MAX_LINES 32  ///< Queued dialogue lines per conversation.
#define RT_GAME3D_DLG_MAX_CHOICES 8 ///< Options per choice prompt.
#define RT_GAME3D_DLG_NAME_MAX 64   ///< Speaker-name capacity.

#define RT_GAME3D_LS_MAX_SHAPES 4 ///< Mouth shapes per LipSync3D binding.

/// @brief One bound mouth shape (name + per-shape weight scale).
typedef struct rt_game3d_ls_shape {
    char name[RT_GAME3D_DLG_NAME_MAX];
    double scale;
    rt_string name_interned; ///< Retained shape name; avoids a per-frame rt_const_cstr alloc.
} rt_game3d_ls_shape;

/// @brief LipSync3D payload: amplitude-envelope mouth drive + procedural blink
///   + gaze sugar over LookAt IK.
typedef struct rt_game3d_lipsync {
    void *entity;      ///< Owner backref (plain; cleared at entity teardown).
    void *morph;       ///< Retained MorphTarget3D driven by the bindings.
    void *gaze_solver; ///< Retained LookAt IKSolver3D, or NULL.
    void *gaze_target; ///< Retained Vec3 target handed to the solver.
    rt_game3d_ls_shape shapes[RT_GAME3D_LS_MAX_SHAPES];
    int32_t shape_count;
    int64_t voice_id; ///< Metered voice being tracked, or -1.
    double envelope;  ///< Smoothed level (attack 0.04 s / release 0.12 s).
    int8_t driving;   ///< Voice drive active.
    /* Blink layer (seeded LCG so replays match). */
    int8_t blink_enabled;
    char blink_shape[RT_GAME3D_DLG_NAME_MAX];
    rt_string blink_interned; ///< Retained blink shape name; avoids a per-frame alloc.
    double blink_min_interval;
    double blink_max_interval;
    double blink_timer;  ///< Countdown to the next blink.
    double blink_phase;  ///< Active blink progress (0 = idle).
    uint64_t blink_seed; ///< LCG state.
    double gaze_weight;  ///< Eased IK weight.
    int8_t gaze_active;
} rt_game3d_lipsync;

/// @brief One queued dialogue line (text resolved at say() time).
typedef struct rt_game3d_dlg_line {
    char speaker[RT_GAME3D_DLG_NAME_MAX];
    char text[RT_GAME3D_TL_TEXT_MAX];
    void *voice_clip; ///< Retained clip played when the line starts, or NULL.
} rt_game3d_dlg_line;

/// @brief Dialogue3D payload: line queue + typewriter reveal + choice prompt +
///   speaker anchoring + localization binding + style knobs.
typedef struct rt_game3d_dialogue {
    void *world;          ///< Retained world back-ref.
    void *bundle;         ///< Retained MessageBundle for key resolution, or NULL.
    void *speaker_entity; ///< Retained anchor entity, or NULL.
    rt_game3d_dlg_line lines[RT_GAME3D_DLG_MAX_LINES];
    int32_t line_count;
    int32_t line_index;
    double reveal_chars;   ///< Characters revealed on the current line.
    double reveal_speed;   ///< Characters per second (default 40).
    double hold_remaining; ///< Auto-advance hold after the reveal completes.
    int8_t active;         ///< Shown and consuming the overlay.
    int8_t anchored;       ///< Bubble above the speaker entity when visible.
    int8_t auto_advance;   ///< Advance lines automatically after reveal + hold.
    int8_t line_started;   ///< Voice fired for the current line.
    char choices[RT_GAME3D_DLG_MAX_CHOICES][RT_GAME3D_TL_TEXT_MAX];
    int32_t choice_count;
    int32_t choice_selected;
    int8_t choice_active; ///< Blocks advance until confirmed.
    int8_t choice_made;   ///< One-shot: a choice was confirmed.
    int64_t last_choice;  ///< Index confirmed by the last choice prompt.
    double panel_alpha;   ///< Bottom-panel opacity (default 0.65).
    int64_t name_color;   ///< Speaker-name color (default 0xFFD75A).
} rt_game3d_dialogue;

#define RT_GAME3D_RAIL_MAX_KEYS 16 ///< FOV/roll keys per rail camera.

/// @brief One rail-camera key: value at arclength-normalized t.
typedef struct rt_game3d_rail_key {
    double t;
    double value;
} rt_game3d_rail_key;

/// @brief RailCamera3D payload: owning world, spline path, look target (entity,
///   point, or second path), progress/auto-advance state, damping, and
///   piecewise FOV/roll keys.
typedef struct rt_game3d_rail_camera {
    void *world;             ///< Retained world back-ref (controller-slot pattern).
    void *path;              ///< Retained Path3D the camera rides.
    void *look_entity;       ///< Retained Entity3D look target, or NULL.
    void *look_point;        ///< Retained Vec3 look target, or NULL.
    void *look_path;         ///< Retained Path3D look target, or NULL.
    double progress;         ///< Requested arclength-normalized position [0,1].
    double smoothed;         ///< Damped progress actually applied.
    double speed;            ///< Auto-advance in units/sec along arclength (0 = manual).
    double position_damping; ///< Exponential smoothing for progress jumps (0 = snap).
    int8_t key_ease;         ///< 0 = linear keys, 1 = smoothstep between keys.
    rt_game3d_rail_key fov_keys[RT_GAME3D_RAIL_MAX_KEYS];
    int32_t fov_key_count;
    rt_game3d_rail_key roll_keys[RT_GAME3D_RAIL_MAX_KEYS];
    int32_t roll_key_count;
} rt_game3d_rail_camera;

/// @brief TargetLock3D payload: owning world, owner entity (the player), the
///   currently locked target, acquisition tuning (range/cone/mask/LoS), and the
///   one-shot acquired/lost polling flags with the LoS grace timer.
typedef struct rt_game3d_targetlock {
    void *world;              ///< Retained World3D back-ref.
    void *owner;              ///< Retained owner Entity3D (scoring origin).
    void *target;             ///< Retained locked Entity3D or NULL.
    double max_distance;      ///< Acquisition radius.
    double cone_degrees;      ///< Half-angle cone from camera forward.
    int64_t candidate_mask;   ///< Layers that are targetable.
    int8_t require_los;       ///< Reject candidates without line of sight.
    double stickiness;        ///< Score multiplier for the current target.
    double break_distance;    ///< Auto-release distance.
    double los_grace_seconds; ///< LoS-break grace before auto-release.
    double los_broken_time;   ///< Accumulated seconds the LoS has been broken.
    int8_t just_acquired;     ///< One-shot poll flag set on acquisition.
    int8_t just_lost;         ///< One-shot poll flag set on release.
    void *candidate_scratch;  ///< Owned reusable target-lock candidate records.
    int32_t candidate_capacity;
    void **candidate_seen; ///< Owned reusable open-addressed entity identity set.
    int32_t candidate_seen_capacity;
} rt_game3d_targetlock;

#define RT_GAME3D_HITBOX_MAX_WINDOWS 4     ///< Animation windows per hitbox.
#define RT_GAME3D_HITBOX_MAX_VICTIMS 16    ///< Rehit-suppression ring per activation.
#define RT_GAME3D_HITBOX_STATE_NAME_MAX 64 ///< Window state-name capacity.
#define RT_GAME3D_HITBOX_KIND_HURT 0       ///< Damageable region volume.
#define RT_GAME3D_HITBOX_KIND_HIT 1        ///< Attack volume.

/// @brief One animation-window binding: the hitbox is live while the owner's
///   animator base state matches @p state and its time is within [t0, t1].
typedef struct rt_game3d_hitbox_window {
    char state[RT_GAME3D_HITBOX_STATE_NAME_MAX];
    double t0;
    double t1;
} rt_game3d_hitbox_window;

/// @brief Hitbox3D payload: owner backref (plain pointer cleared at entity
///   teardown), retained collider shape, bone/entity attachment, combat filters,
///   activation state, window bindings, and the per-activation rehit ring.
typedef struct rt_game3d_hitbox {
    struct rt_game3d_entity *entity; ///< Owner; NULLed when the entity tears down.
    void *collider;                  ///< Retained Collider3D shape.
    int64_t bone_index;              ///< -1 = entity-space attachment.
    double local_offset[3];          ///< Offset in bone/entity space.
    int8_t kind;                     ///< RT_GAME3D_HITBOX_KIND_*.
    int64_t team;                    ///< Same-team pairs are skipped unless friendly fire.
    int64_t channel;                 ///< Bitmask; hit×hurt require overlapping channels.
    int8_t friendly_fire;            ///< Allow same-team hits from this attacker.
    int8_t active;                   ///< Manual activation switch.
    int8_t was_live;                 ///< Previous-step liveness (rehit reset edge).
    rt_game3d_hitbox_window windows[RT_GAME3D_HITBOX_MAX_WINDOWS];
    int32_t window_count;
    /// Previous liveness sample so a coarse step cannot jump OVER a narrow
    /// window (liveness tests [prev_time, now] crossing, loop-aware).
    double window_prev_time;
    int8_t window_prev_valid;
    int8_t window_prev_playing[RT_GAME3D_HITBOX_MAX_WINDOWS];
    /// Victims already hit during the current activation (one hit per swing).
    struct rt_game3d_entity *hit_victims[RT_GAME3D_HITBOX_MAX_VICTIMS];
    int32_t hit_victim_count;
} rt_game3d_hitbox;

/// @brief Health3D payload: owner backref (plain pointer cleared at entity
///   teardown), hit points, i-frame state, and one-shot damage/death flags.
typedef struct rt_game3d_health {
    struct rt_game3d_entity *entity; ///< Owner; NULLed when the entity tears down.
    double max_hp;
    double hp;
    double invuln_seconds;   ///< I-frame duration granted per applied damage.
    double invuln_remaining; ///< Ticked down by the world combat pass.
    int8_t dead;             ///< Latched at hp <= 0 until Revive.
    int8_t just_died;        ///< One-shot flag, cleared next combat pass.
    int8_t just_damaged;     ///< One-shot flag, cleared next combat pass.
    double last_damage;      ///< Most recent applied amount.
    int64_t last_tag;        ///< Caller-supplied damage tag.
} rt_game3d_health;

/// @brief One buffered hit event: retained handles released when the buffer clears.
typedef struct rt_game3d_hit_event_rec {
    void *attacker; ///< Retained Entity3D.
    void *victim;   ///< Retained Entity3D.
    void *hitbox;   ///< Retained attacking Hitbox3D.
    void *hurtbox;  ///< Retained victim Hitbox3D.
    double point[3];
    double normal[3];
} rt_game3d_hit_event_rec;

/// @brief One buffered damage event: retained handles released when the buffer clears.
typedef struct rt_game3d_damage_event_rec {
    void *victim; ///< Retained Entity3D.
    void *source; ///< Retained Entity3D or NULL.
    double amount;
    int64_t tag;
    int8_t was_lethal;
} rt_game3d_damage_event_rec;

/// @brief Boxed HitEvent3D handle returned by World3D.hitEvent (fail-closed).
typedef struct rt_game3d_hit_event {
    void *attacker;
    void *victim;
    void *hitbox;
    void *hurtbox;
    double point[3];
    double normal[3];
} rt_game3d_hit_event;

/// @brief Boxed DamageEvent3D handle returned by World3D.damageEvent (fail-closed).
typedef struct rt_game3d_damage_event {
    void *victim;
    void *source;
    double amount;
    int64_t tag;
    int8_t was_lethal;
} rt_game3d_damage_event;

/// @brief One body→entity index entry, mapping a physics body handle to the
///   Entity3D that owns it for fast collision-event entity lookup.
typedef struct {
    void *body;
    rt_game3d_entity *entity;
} rt_game3d_body_index_entry;

/// @brief World3D payload: the owned canvas/camera/scene/physics/input/audio/
///   effects subsystems, the active camera controller, the spawned-entity list,
///   per-frame timing/frame counters, window size, clear color, and the set of
///   debug-overlay toggles plus the destroyed flag.
typedef struct rt_game3d_world {
    void *canvas;
    void *camera;
    void *scene;
    void *physics;
    void *input;
    void *audio;
    void *effects;
    void *stream;
    void *camera_controller;
    rt_game3d_entity **entities;
    int32_t entity_count;
    int32_t entity_capacity;
    rt_game3d_body_index_entry *body_index_entries;
    int32_t body_index_count;
    int32_t body_index_capacity;
    uint64_t *name_index_hashes;
    rt_game3d_entity **name_index_entities;
    int32_t name_index_count;
    int32_t name_index_capacity;
    int8_t name_index_valid;
    void **animation_animators;
    int32_t animation_animator_capacity;
    void **animation_seen_set;
    size_t animation_seen_capacity;
    void *animation_jobs;
    int32_t animation_job_capacity;
    int64_t next_entity_id;
    double dt;
    double elapsed;
    int64_t frame;
    int64_t dropped_fixed_steps;
    double time_scale;        /* world time multiplier, clamped [0, 4], default 1 */
    int8_t paused;            /* latched pause: effective scale 0 while set */
    double hitstop_remaining; /* one-shot freeze, decays by REAL (unscaled) dt */
    double unscaled_dt;       /* real clamped frame step (UI/menus) */
    double unscaled_elapsed;  /* real elapsed seconds (UI/menus) */
    int64_t tick_frame_stamp; /* frame index stamped by Update(); StepSimulation
                                 detects the documented combined loop and skips
                                 the per-frame accounting Update already did */
    int64_t worker_count;
    void *job_pool;
    int8_t jobs_enabled;
    int8_t floating_origin;
    double origin_rebase_threshold;
    double world_origin[3];
    int64_t width;
    int64_t height;
    double clear_r;
    double clear_g;
    double clear_b;
    void *active_timeline;               /* retained Timeline3D playing in this world, or NULL */
    void *active_dialogue;               /* retained Dialogue3D shown in this world, or NULL */
    rt_game3d_hit_event_rec *hit_events; /* combat-pass hit buffer, cleared each step */
    int32_t hit_event_count;
    int32_t hit_event_capacity;
    void *combat_scratch; /* lazily allocated per-world combat volume scratch (combat.c) */
    rt_game3d_damage_event_rec *damage_events; /* damage buffer, cleared each step */
    int32_t damage_event_count;
    int32_t damage_event_capacity;
    void *debug_axis_origin;
    double debug_axis_size;
    double stream_camera_user_far;
    double stream_camera_effective_far;
    int8_t debug_overlay_enabled;
    int8_t debug_axes_enabled;
    int8_t debug_physics_enabled;
    int8_t debug_camera_enabled;
    int8_t debug_caps_enabled;
    int8_t stream_camera_far_active;
    int8_t destroyed;
    double fixed_interpolation_alpha;
    /* Opt-in fixed-step render interpolation: entity node poses are blended between
     * the previous and current fixed steps by fixed_interpolation_alpha during render,
     * then restored — 60 Hz physics stays smooth on 120/144 Hz displays. */
    int8_t render_interpolation;
    void **cloths;                             /* retained Cloth3D handles ticked per step */
    int32_t cloth_count;                       /* number of registered cloths */
    int32_t cloth_capacity;                    /* cloth array capacity */
    rt_game3d_persist_record *persist_records; /* keyed entity-state deltas (plan 17) */
    int32_t persist_count;                     /* number of records */
    int32_t persist_capacity;                  /* record array capacity */
    rt_game3d_hitch_entry hitches[RT_GAME3D_MAX_HITCHES]; /* hitch ring (plan 30) */
    int32_t hitch_count;               /* live entries (<= RT_GAME3D_MAX_HITCHES) */
    int32_t hitch_head;                /* oldest entry index once the ring wraps */
    double hitch_threshold_ms;         /* FrameTotal threshold (default 25) */
    double hitch_last_stream_stall_ms; /* stream stall watermark at last step */
    /* Monotonic stamp for despawn-safe entity sweeps (see
     * game3d_world_sweep_entities): each sweep bumps it; entities record the
     * stamp when ticked so swap-remove compaction can neither double-tick a
     * survivor nor skip one moved into an already-visited slot. Appended at
     * the end: test fixtures mirror prefixes of this layout. */
    uint32_t sim_tick_stamp;
    /// Authoritative allocation metadata for raw world-owned arrays. Mirrored
    /// capacities are repairable state and are never trusted for realloc/free
    /// without these address-bound cookies.
    int32_t entity_storage_capacity;
    uint64_t entity_storage_cookie;
    int32_t animation_animator_storage_capacity;
    uint64_t animation_animator_storage_cookie;
    size_t animation_seen_storage_capacity;
    uint64_t animation_seen_storage_cookie;
    int32_t animation_job_storage_capacity;
    uint64_t animation_job_storage_cookie;
} rt_game3d_world;

#if defined(_MSC_VER)
#define RT_GAME3D_THREAD_LOCAL __declspec(thread)
#else
#define RT_GAME3D_THREAD_LOCAL _Thread_local
#endif

/// @brief Build a stable lifetime diagnostic in the public `Game3D.Type.method: reason` form.
/// @details `method` strings often include an invalid-handle suffix; lifetime traps replace that
///          suffix with the actual destroyed-handle reason while keeping the qualified API name.
/// @param method Qualified API name, optionally followed by an existing diagnostic suffix.
/// @param reason Replacement lifetime failure reason; empty values use a generic fallback.
/// @return Pointer to thread-local storage valid until the next call on the same thread.
static inline const char *game3d_lifetime_diag(const char *method, const char *reason) {
    static RT_GAME3D_THREAD_LOCAL char message[256];
    const char *fallback_method = "Game3D";
    const char *fallback_reason = "invalid lifetime";
    const char *qualified = method && method[0] ? method : fallback_method;
    const char *why = reason && reason[0] ? reason : fallback_reason;
    const char *suffix = strchr(qualified, ':');
    int method_len = suffix ? (int)(suffix - qualified) : (int)strlen(qualified);
    if (method_len < 0)
        method_len = 0;
    snprintf(message, sizeof(message), "%.*s: %s", method_len, qualified, why);
    message[sizeof(message) - 1] = '\0';
    return message;
}

//=========================================================================
// Handle validators — each downcasts an opaque handle to its typed payload,
// trapping `method` (the caller's qualified API name) on a class-id mismatch
// or NULL handle. They centralize the "untrusted handle in, trusted pointer
// out" contract every public entry point relies on.
//=========================================================================

/// @brief Validate `obj` as a LayerMask handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed LayerMask payload, or `NULL` after recording the trap.
static inline rt_game3d_layermask *game3d_layermask_checked(void *obj, const char *method) {
    rt_game3d_layermask *mask =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_LAYERMASK_CLASS_ID, sizeof(rt_game3d_layermask))
            ? (rt_game3d_layermask *)obj
            : NULL;
    if (!mask)
        rt_trap(method);
    return mask;
}

/// @brief Validate `obj` as an Input3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed Input3D payload, or `NULL` after recording the trap.
static inline rt_game3d_input *game3d_input_checked(void *obj, const char *method) {
    rt_game3d_input *input =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_INPUT_CLASS_ID, sizeof(rt_game3d_input))
            ? (rt_game3d_input *)obj
            : NULL;
    if (!input)
        rt_trap(method);
    return input;
}

/// @brief Validate `obj` as an Entity3D handle (allowing a despawned/destroyed one),
///   trapping `method` on class mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when class validation fails.
/// @return Typed Entity3D payload regardless of lifetime state, or `NULL` after a trap.
static inline rt_game3d_entity *game3d_entity_checked_allow_destroyed(void *obj,
                                                                      const char *method) {
    rt_game3d_entity *entity =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_ENTITY_CLASS_ID, sizeof(rt_game3d_entity))
            ? (rt_game3d_entity *)obj
            : NULL;
    if (!entity)
        rt_trap(method);
    return entity;
}

/// @brief True when a typed Entity3D payload is still live for public API access.
/// @param entity Borrowed typed entity payload, or `NULL`.
/// @return Nonzero for a live entity; zero after recording a stale-handle call when applicable.
static inline int game3d_entity_alive_or_record(const rt_game3d_entity *entity) {
    if (!entity)
        return 0;
    if (!entity->alive || entity->destroyed) {
        rt_game3d_diag_record_stale_entity_call();
        return 0;
    }
    return 1;
}

/// @brief Mark a retained Entity3D handle as dead after it leaves its world.
/// @param entity Entity payload to invalidate; `NULL` is ignored.
static inline void game3d_entity_mark_dead(rt_game3d_entity *entity) {
    if (!entity)
        return;
    entity->alive = 0;
    entity->spawned = 0;
    entity->destroyed = 1;
    entity->world = NULL;
    entity->registry_index = -1;
}

/// @brief Validate `obj` as a live Entity3D handle; stale handles record diagnostics
///   and resolve to NULL so callers can return neutral values or no-op.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when class validation fails.
/// @return Typed live Entity3D payload, or `NULL` for invalid or stale input.
static inline rt_game3d_entity *game3d_entity_checked(void *obj, const char *method) {
    rt_game3d_entity *entity = game3d_entity_checked_allow_destroyed(obj, method);
    if (!game3d_entity_alive_or_record(entity))
        return NULL;
    return entity;
}

/// @brief Validate `obj` as a Sound3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed Sound3D payload, or `NULL` after recording the trap.
static inline void game3d_audio_repair_scalar_state(rt_game3d_audio *audio) {
    if (!audio)
        return;
    if (!isfinite(audio->ref_distance) || audio->ref_distance <= 0.0)
        audio->ref_distance = RT_GAME3D_DEFAULT_AUDIO_REF_DISTANCE;
    else if (audio->ref_distance > RT_GAME3D_AUDIO_DISTANCE_MAX)
        audio->ref_distance = RT_GAME3D_AUDIO_DISTANCE_MAX;
    if (!isfinite(audio->max_distance) || audio->max_distance <= 0.0)
        audio->max_distance = RT_GAME3D_DEFAULT_AUDIO_MAX_DISTANCE;
    else if (audio->max_distance > RT_GAME3D_AUDIO_DISTANCE_MAX)
        audio->max_distance = RT_GAME3D_AUDIO_DISTANCE_MAX;
    if (audio->max_distance < audio->ref_distance)
        audio->max_distance = audio->ref_distance;
    if (audio->volume < 0)
        audio->volume = 0;
    else if (audio->volume > 100)
        audio->volume = 100;
    audio->listener_follow_camera = audio->listener_follow_camera ? 1 : 0;
    audio->reverb_routing = audio->reverb_routing ? 1 : 0;
    audio->occlusion_enabled = audio->occlusion_enabled ? 1 : 0;
    if (!isfinite(audio->reverb_blend) || audio->reverb_blend < 0.0)
        audio->reverb_blend = 0.5;
    if (!isfinite(audio->reverb_room))
        audio->reverb_room = 0.5;
    else if (audio->reverb_room < 0.0)
        audio->reverb_room = 0.0;
    else if (audio->reverb_room > 1.0)
        audio->reverb_room = 1.0;
    if (!isfinite(audio->reverb_damp))
        audio->reverb_damp = 0.5;
    else if (audio->reverb_damp < 0.0)
        audio->reverb_damp = 0.0;
    else if (audio->reverb_damp > 1.0)
        audio->reverb_damp = 1.0;
    if (!isfinite(audio->reverb_wet) || audio->reverb_wet < 0.0)
        audio->reverb_wet = 0.0;
    else if (audio->reverb_wet > 1.0)
        audio->reverb_wet = 1.0;
    if (!isfinite(audio->occlusion_amount))
        audio->occlusion_amount = 1.0;
    else if (audio->occlusion_amount < 0.0)
        audio->occlusion_amount = 0.0;
    else if (audio->occlusion_amount > 1.0)
        audio->occlusion_amount = 1.0;
    if (audio->occlusion_budget <= 0)
        audio->occlusion_budget = 8;
    else if (audio->occlusion_budget > 256)
        audio->occlusion_budget = 256;
    if (audio->occlusion_cursor < 0)
        audio->occlusion_cursor = 0;
    if (audio->reverb_group < -1)
        audio->reverb_group = -1;
    if (audio->reverb_fx < -1)
        audio->reverb_fx = -1;
    if (audio->dialogue_group < -1)
        audio->dialogue_group = -1;
}

static inline rt_game3d_audio *game3d_audio_checked(void *obj, const char *method) {
    rt_game3d_audio *audio =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_SOUND_CLASS_ID, sizeof(rt_game3d_audio))
            ? (rt_game3d_audio *)obj
            : NULL;
    if (!audio)
        rt_trap(method);
    else
        game3d_audio_repair_scalar_state(audio);
    return audio;
}

/// @brief Validate `obj` as an EffectRegistry3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed EffectRegistry3D payload, or `NULL` after recording the trap.
static inline rt_game3d_effects *game3d_effects_checked(void *obj, const char *method) {
    rt_game3d_effects *effects =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_EFFECTS_CLASS_ID, sizeof(rt_game3d_effects))
            ? (rt_game3d_effects *)obj
            : NULL;
    if (!effects)
        rt_trap(method);
    return effects;
}

/// @brief Validate `obj` as an EnvHandle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed environment-handle payload, or `NULL` after recording the trap.
static inline rt_game3d_env_handle *game3d_env_handle_checked(void *obj, const char *method) {
    rt_game3d_env_handle *env =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_ENV_HANDLE_CLASS_ID, sizeof(rt_game3d_env_handle))
            ? (rt_game3d_env_handle *)obj
            : NULL;
    if (!env)
        rt_trap(method);
    return env;
}

/// @brief Validate `obj` as a BodyDef handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed BodyDef payload, or `NULL` after recording the trap.
static inline rt_game3d_body_def *game3d_body_def_checked(void *obj, const char *method) {
    rt_game3d_body_def *def =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_BODYDEF_CLASS_ID, sizeof(rt_game3d_body_def))
            ? (rt_game3d_body_def *)obj
            : NULL;
    if (!def)
        rt_trap(method);
    return def;
}

/// @brief Validate `obj` as a Collision3DEvent handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed collision-event payload, or `NULL` after recording the trap.
static inline rt_game3d_collision_event *game3d_collision_event_checked(void *obj,
                                                                        const char *method) {
    rt_game3d_collision_event *event = rt_obj_is_instance(obj,
                                                          RT_G3D_GAME3D_COLLISION_EVENT_CLASS_ID,
                                                          sizeof(rt_game3d_collision_event))
                                           ? (rt_game3d_collision_event *)obj
                                           : NULL;
    if (!event)
        rt_trap(method);
    return event;
}

/// @brief Validate `obj` as an Animator3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed Animator3D payload, or `NULL` after recording the trap.
static inline rt_game3d_animator *game3d_animator_checked(void *obj, const char *method) {
    rt_game3d_animator *animator =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_ANIMATOR3D_CLASS_ID, sizeof(rt_game3d_animator))
            ? (rt_game3d_animator *)obj
            : NULL;
    if (!animator)
        rt_trap(method);
    return animator;
}

/// @brief Validate `obj` as a ModelTemplate handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed ModelTemplate payload, or `NULL` after recording the trap.
static inline rt_game3d_model_template *game3d_model_template_checked(void *obj,
                                                                      const char *method) {
    rt_game3d_model_template *model_template =
        rt_obj_is_instance(
            obj, RT_G3D_GAME3D_MODEL_TEMPLATE_CLASS_ID, sizeof(rt_game3d_model_template))
            ? (rt_game3d_model_template *)obj
            : NULL;
    if (!model_template)
        rt_trap(method);
    return model_template;
}

/// @brief Validate `obj` as an AssetHandle3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed AssetHandle3D payload, or `NULL` after recording the trap.
static inline rt_game3d_asset_handle *game3d_asset_handle_checked(void *obj, const char *method) {
    rt_game3d_asset_handle *handle = rt_obj_is_instance(obj,
                                                        RT_G3D_GAME3D_ASSET_HANDLE3D_CLASS_ID,
                                                        sizeof(rt_game3d_asset_handle))
                                         ? (rt_game3d_asset_handle *)obj
                                         : NULL;
    if (!handle)
        rt_trap(method);
    return handle;
}

/// @brief Validate `obj` as a WorldStream3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed WorldStream3D payload, or `NULL` after recording the trap.
static inline rt_game3d_world_stream *game3d_world_stream_checked(void *obj, const char *method) {
    rt_game3d_world_stream *stream = rt_obj_is_instance(obj,
                                                        RT_G3D_GAME3D_WORLD_STREAM3D_CLASS_ID,
                                                        sizeof(rt_game3d_world_stream))
                                         ? (rt_game3d_world_stream *)obj
                                         : NULL;
    if (!stream)
        rt_trap(method);
    return stream;
}

/// @brief Validate `obj` as a World3D handle (allowing a destroyed one), trapping
///   `method` on class mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when class validation fails.
/// @return Typed World3D payload regardless of lifetime state, or `NULL` after a trap.
static inline rt_game3d_world *game3d_world_checked_allow_destroyed(void *obj, const char *method) {
    rt_game3d_world *world =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_WORLD_CLASS_ID, sizeof(rt_game3d_world))
            ? (rt_game3d_world *)obj
            : NULL;
    if (!world)
        rt_trap(method);
    return world;
}

/// @brief Validate `obj` as a live World3D handle, additionally trapping if the
///   world has been destroyed.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Qualified API diagnostic used for invalid or destroyed handles.
/// @return Typed live World3D payload, or `NULL` after recording a trap.
static inline rt_game3d_world *game3d_world_checked(void *obj, const char *method) {
    rt_game3d_world *world = game3d_world_checked_allow_destroyed(obj, method);
    if (world && world->destroyed) {
        rt_trap(game3d_lifetime_diag(method, "world is destroyed"));
        return NULL;
    }
    return world;
}

/// @brief Validate `obj` as a CharacterController3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed CharacterController3D payload, or `NULL` after recording the trap.
static inline rt_game3d_character_controller *game3d_character_controller_checked(
    void *obj, const char *method) {
    rt_game3d_character_controller *controller =
        rt_obj_is_instance(obj,
                           RT_G3D_GAME3D_CHARACTER_CONTROLLER_CLASS_ID,
                           sizeof(rt_game3d_character_controller))
            ? (rt_game3d_character_controller *)obj
            : NULL;
    if (!controller)
        rt_trap(method);
    return controller;
}

/// @brief Validate `obj` as a FirstPersonController handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed FirstPersonController payload, or `NULL` after recording the trap.
static inline rt_game3d_first_person_controller *game3d_first_person_controller_checked(
    void *obj, const char *method) {
    rt_game3d_first_person_controller *controller =
        rt_obj_is_instance(
            obj, RT_G3D_GAME3D_FIRSTPERSON_CLASS_ID, sizeof(rt_game3d_first_person_controller))
            ? (rt_game3d_first_person_controller *)obj
            : NULL;
    if (!controller)
        rt_trap(method);
    return controller;
}

/// @brief Validate `obj` as a FreeFlyController handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed FreeFlyController payload, or `NULL` after recording the trap.
static inline rt_game3d_free_fly_controller *game3d_free_fly_controller_checked(
    void *obj, const char *method) {
    rt_game3d_free_fly_controller *controller =
        rt_obj_is_instance(
            obj, RT_G3D_GAME3D_FREEFLY_CLASS_ID, sizeof(rt_game3d_free_fly_controller))
            ? (rt_game3d_free_fly_controller *)obj
            : NULL;
    if (!controller)
        rt_trap(method);
    return controller;
}

/// @brief Validate `obj` as an OrbitController handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed OrbitController payload, or `NULL` after recording the trap.
static inline rt_game3d_orbit_controller *game3d_orbit_controller_checked(void *obj,
                                                                          const char *method) {
    rt_game3d_orbit_controller *controller =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_ORBIT_CLASS_ID, sizeof(rt_game3d_orbit_controller))
            ? (rt_game3d_orbit_controller *)obj
            : NULL;
    if (!controller)
        rt_trap(method);
    return controller;
}

/// @brief Validate `obj` as a FollowController handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed FollowController payload, or `NULL` after recording the trap.
static inline rt_game3d_follow_controller *game3d_follow_controller_checked(void *obj,
                                                                            const char *method) {
    rt_game3d_follow_controller *controller =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_FOLLOW_CLASS_ID, sizeof(rt_game3d_follow_controller))
            ? (rt_game3d_follow_controller *)obj
            : NULL;
    if (!controller)
        rt_trap(method);
    return controller;
}

/// @brief Repair a validated third-person controller's private numeric and reference state.
/// @param controller Complete ThirdPersonController payload; NULL is ignored.
void game3d_thirdperson_repair_state(rt_game3d_thirdperson_controller *controller);

/// @brief Validate `obj` as a ThirdPersonController handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed ThirdPersonController payload, or `NULL` after recording the trap.
static inline rt_game3d_thirdperson_controller *game3d_thirdperson_controller_checked(
    void *obj, const char *method) {
    rt_game3d_thirdperson_controller *controller =
        rt_obj_is_instance(
            obj, RT_G3D_GAME3D_THIRDPERSON_CLASS_ID, sizeof(rt_game3d_thirdperson_controller))
            ? (rt_game3d_thirdperson_controller *)obj
            : NULL;
    if (!controller)
        rt_trap(method);
    if (controller)
        game3d_thirdperson_repair_state(controller);
    return controller;
}

/// @brief Validate `obj` as a LipSync3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed LipSync3D payload, or `NULL` after recording the trap.
static inline rt_game3d_lipsync *game3d_lipsync_checked(void *obj, const char *method) {
    rt_game3d_lipsync *lipsync =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_LIPSYNC_CLASS_ID, sizeof(rt_game3d_lipsync))
            ? (rt_game3d_lipsync *)obj
            : NULL;
    if (!lipsync)
        rt_trap(method);
    return lipsync;
}

/// @brief Validate `obj` as a Dialogue3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed Dialogue3D payload, or `NULL` after recording the trap.
static inline rt_game3d_dialogue *game3d_dialogue_checked(void *obj, const char *method) {
    rt_game3d_dialogue *dialogue =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_DIALOGUE_CLASS_ID, sizeof(rt_game3d_dialogue))
            ? (rt_game3d_dialogue *)obj
            : NULL;
    if (!dialogue)
        rt_trap(method);
    return dialogue;
}

/// @brief Validate `obj` as a Timeline3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed Timeline3D payload, or `NULL` after recording the trap.
static inline rt_game3d_timeline *game3d_timeline_checked(void *obj, const char *method) {
    rt_game3d_timeline *timeline =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_TIMELINE_CLASS_ID, sizeof(rt_game3d_timeline))
            ? (rt_game3d_timeline *)obj
            : NULL;
    if (!timeline)
        rt_trap(method);
    return timeline;
}

/// @brief Validate `obj` as a RailCamera3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed RailCamera3D payload, or `NULL` after recording the trap.
static inline rt_game3d_rail_camera *game3d_rail_camera_checked(void *obj, const char *method) {
    rt_game3d_rail_camera *rail =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_RAILCAMERA_CLASS_ID, sizeof(rt_game3d_rail_camera))
            ? (rt_game3d_rail_camera *)obj
            : NULL;
    if (!rail)
        rt_trap(method);
    return rail;
}

/// @brief Repair a validated target-lock payload's private numeric and reference state.
/// @param lock Complete TargetLock3D payload; NULL is ignored.
void game3d_targetlock_repair_state(rt_game3d_targetlock *lock);

/// @brief Validate `obj` as a TargetLock3D handle, trapping `method` on mismatch.
/// @param obj Opaque handle supplied by a public API caller.
/// @param method Diagnostic message used when validation fails.
/// @return Typed TargetLock3D payload, or `NULL` after recording the trap.
static inline rt_game3d_targetlock *game3d_targetlock_checked(void *obj, const char *method) {
    rt_game3d_targetlock *lock =
        rt_obj_is_instance(obj, RT_G3D_GAME3D_TARGETLOCK_CLASS_ID, sizeof(rt_game3d_targetlock))
            ? (rt_game3d_targetlock *)obj
            : NULL;
    if (!lock)
        rt_trap(method);
    if (lock)
        game3d_targetlock_repair_state(lock);
    return lock;
}

//=========================================================================
// Shared internal helpers — defined in rt_game3d.c / rt_game3d_controllers.c,
// declared here for the split sibling translation units.
//=========================================================================
/// @brief Replace an untyped retained-reference slot.
/// @details Retains @p value before releasing the slot's previous value so self-assignment is safe.
/// @param[in,out] slot Address of the owned reference slot.
/// @param value Borrowed replacement handle, or `NULL` to clear the slot.
void game3d_assign_ref(void **slot, void *value);

/// @brief Release and clear an untyped retained-reference slot.
/// @param[in,out] slot Address of the owned reference slot; `NULL` is ignored.
void game3d_release_ref(void **slot);

/// @brief Replace a retained slot after validating the replacement's runtime class.
/// @param[in,out] slot Address of the owned reference slot.
/// @param value Borrowed replacement handle, or `NULL` to clear the slot.
/// @param class_id Required runtime class identifier for a non-null replacement.
void game3d_assign_typed_ref(void **slot, void *value, int64_t class_id);

/// @brief Release and clear a retained slot when its value still matches a runtime class.
/// @param[in,out] slot Address of the owned reference slot; `NULL` is ignored.
/// @param class_id Runtime class identifier expected in the current slot.
void game3d_release_typed_ref(void **slot, int64_t class_id);

/// @brief Create an Animator3D wrapper that can hold both skeletal and node-animation drivers.
/// @details Used by model instantiation so imported assets with skeletal Animation3D clips,
/// NodeAnimation3D clips, or both expose a single Game3D Animator3D handle to scripts.
/// @param controller Borrowed skeletal Animation3D controller, or `NULL`.
/// @param node_animator Borrowed NodeAnimator3D driver, or `NULL`.
/// @return New GC-managed Animator3D handle, or `NULL` on allocation failure.
void *rt_game3d_animator_new_from_bindings(void *controller, void *node_animator);

/// @brief Return true when a Game3D animator must be advanced during World3D animation jobs.
/// @details NodeAnimator3D-only wrappers are stepped by Scene3D.SyncBindings because they mutate
/// scene node transforms directly; skeletal controllers still run in the Game3D animation phase.
/// @param obj Borrowed Animator3D handle.
/// @return Nonzero when the wrapper contains a skeletal controller that needs a Game3D update.
int8_t rt_game3d_animator_needs_game_update(void *obj);

/// @brief Clamp a scalar to an inclusive interval.
/// @param value Value to bound.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return @p value bounded to `[`@p lo`,` @p hi`]`.
double game3d_clamp(double value, double lo, double hi);

/// @brief Sanitize and cap a simulation delta.
/// @param dt Candidate elapsed time in seconds.
/// @return A finite non-negative delta no greater than `RT_GAME3D_MAX_DT`.
double game3d_clamp_dt(double dt);

/// @brief Sanitize a controller delta while preserving explicit zero-time pauses.
/// @param dt Candidate elapsed time in seconds.
/// @return A finite value in `[0, RT_GAME3D_MAX_DT]`.
double game3d_clamp_controller_dt(double dt);

/// @brief Substitute a fallback for a non-finite scalar.
/// @param value Candidate scalar.
/// @param fallback Value used when @p value is NaN or infinite.
/// @return @p value when finite; otherwise @p fallback.
double game3d_finite_or(double value, double fallback);

/// @brief Sanitize a scalar and clamp its absolute magnitude.
/// @param value Candidate scalar.
/// @param fallback Value used for non-finite input.
/// @param abs_max Non-negative maximum absolute magnitude.
/// @return A finite value in `[-abs_max, abs_max]`.
double game3d_clamp_abs_or(double value, double fallback, double abs_max);

/// @brief Sanitize a world coordinate against the Game3D coordinate limit.
/// @param value Candidate coordinate.
/// @param fallback Value used for non-finite input.
/// @return Finite coordinate bounded by `RT_GAME3D_COORD_ABS_MAX`.
double game3d_clamp_coord_or(double value, double fallback);

/// @brief Sanitize a scale component, using unit scale for non-finite input.
/// @param value Candidate scale component.
/// @return Finite scale bounded by `RT_GAME3D_SCALE_ABS_MAX`, or `1.0` as the fallback.
double game3d_scale_or_unit(double value);

/// @brief Sanitize a scalar and enforce a non-negative result.
/// @param value Candidate scalar.
/// @param fallback Value used for non-finite input.
/// @return The sanitized value clamped to a minimum of zero.
double game3d_nonnegative_or(double value, double fallback);

/// @brief Sanitize a scalar and clamp it to a non-negative upper-bounded interval.
/// @param value Candidate scalar.
/// @param fallback Value used for non-finite input.
/// @param max_value Inclusive upper bound.
/// @return A finite value in `[0, max_value]`.
double game3d_nonnegative_clamped_or(double value, double fallback, double max_value);

/// @brief Sanitize a scalar and substitute the fallback unless it is strictly positive.
/// @param value Candidate scalar.
/// @param fallback Replacement for non-finite, zero, or negative input.
/// @return @p value when finite and positive; otherwise @p fallback.
double game3d_positive_or(double value, double fallback);

/// @brief Sanitize a positive scalar and clamp it to an upper bound.
/// @param value Candidate scalar.
/// @param fallback Replacement for non-finite, zero, or negative input.
/// @param max_value Inclusive upper bound.
/// @return A positive finite value no greater than @p max_value.
double game3d_positive_clamped_or(double value, double fallback, double max_value);

/// @brief Normalize an XZ direction, substituting a fallback direction when degenerate.
/// @param[in,out] x X component to normalize.
/// @param[in,out] z Z component to normalize.
/// @param fallback_x Fallback X component.
/// @param fallback_z Fallback Z component.
void game3d_normalize_xz(double *x, double *z, double fallback_x, double fallback_z);

/// @brief Read the world retained by any supported camera-controller payload.
/// @param controller Borrowed opaque camera-controller handle.
/// @return Borrowed World3D handle, or `NULL` when the controller is invalid or detached.
void *game3d_camera_controller_get_world_ref(void *controller);

/// @brief Bind any supported camera controller to a world reference.
/// @param controller Borrowed opaque camera-controller handle.
/// @param world Borrowed World3D handle to retain, or `NULL` to detach.
void game3d_camera_controller_bind_world_ref(void *controller, void *world);

/// @brief Release the world retained by any supported camera controller.
/// @param controller Borrowed opaque camera-controller handle; invalid handles are ignored.
void game3d_camera_controller_clear_world_ref(void *controller);

/// @brief Release a camera controller's world only when it equals a specified handle.
/// @param controller Borrowed opaque camera-controller handle.
/// @param world Borrowed World3D handle used as the conditional match.
void game3d_camera_controller_clear_world_ref_if(void *controller, void *world);

/// @brief Test whether a handle belongs to a supported Game3D camera-controller class.
/// @param controller Borrowed opaque handle.
/// @return Nonzero for a valid supported controller; otherwise zero.
int game3d_camera_controller_is_valid(void *controller);

/// @brief Trap unless @p controller is detached or bound to @p world.
/// @param controller Borrowed opaque camera-controller handle.
/// @param world Borrowed world expected to own the controller.
/// @param api_name Qualified API name used in the mismatch diagnostic.
/// @return Nonzero when detached or bound to @p world; zero after recording a trap.
int game3d_camera_controller_validate_world(void *controller,
                                            rt_game3d_world *world,
                                            const char *api_name);

/// @brief Trap unless the CharacterController3D's retained world matches @p world.
/// @param controller Borrowed character-controller payload.
/// @param world Borrowed world expected to own the controller.
/// @param api_name Qualified API name used in the mismatch diagnostic.
/// @return Nonzero when the ownership relation is valid; zero after recording a trap.
int game3d_character_controller_validate_world(rt_game3d_character_controller *controller,
                                               rt_game3d_world *world,
                                               const char *api_name);

/// @brief Trap when a spawned entity belongs to a different world than @p world.
/// @param entity Borrowed entity payload to validate.
/// @param world Borrowed world expected to own the spawned entity.
/// @param api_name Qualified API name used in the mismatch diagnostic.
/// @return Nonzero when unspawned or owned by @p world; zero after recording a trap.
int game3d_entity_validate_controller_world(rt_game3d_entity *entity,
                                            rt_game3d_world *world,
                                            const char *api_name);

/// @brief Resolve an entity node's world-space position.
/// @param entity Borrowed entity payload.
/// @param[out] out_pos Required three-component destination.
/// @return Nonzero on success; zero when the entity has no valid node or position.
int game3d_entity_world_position_components(rt_game3d_entity *entity, double out_pos[3]);

/// @brief Read the horizontal integral mouse delta from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Horizontal mouse delta in pixels, or zero for invalid input.
int64_t game3d_input_mouse_dx(const rt_game3d_input *input);

/// @brief Read the horizontal sub-pixel mouse delta from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Horizontal floating-point mouse delta, or zero for invalid input.
double game3d_input_mouse_fdx(const rt_game3d_input *input);

/// @brief Read the vertical sub-pixel mouse delta from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Vertical floating-point mouse delta, or zero for invalid input.
double game3d_input_mouse_fdy(const rt_game3d_input *input);

/// @brief Read the vertical integral mouse delta from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Vertical mouse delta in pixels, or zero for invalid input.
int64_t game3d_input_mouse_dy(const rt_game3d_input *input);

/// @brief Read the absolute window-local cursor X from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Cursor X in pixels, or zero for invalid input.
int64_t game3d_input_mouse_x(const rt_game3d_input *input);

/// @brief Read the absolute window-local cursor Y from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Cursor Y in pixels, or zero for invalid input.
int64_t game3d_input_mouse_y(const rt_game3d_input *input);

/// @brief Merge keyboard and bound-gamepad movement into a normalized three-axis vector.
/// @param input Borrowed Input3D payload.
/// @param[out] out_x Required lateral-axis destination.
/// @param[out] out_y Required vertical-axis destination.
/// @param[out] out_z Required forward-axis destination.
void game3d_input_move_axis_components(rt_game3d_input *input,
                                       double *out_x,
                                       double *out_y,
                                       double *out_z);

/// @brief Read the vertical wheel delta from an Input3D snapshot or live input.
/// @param input Borrowed Input3D payload.
/// @return Vertical wheel delta, or zero for invalid input.
double game3d_input_wheel_y_snapshot(const rt_game3d_input *input);

/// @brief Repair scalar, flag, and gamepad snapshot invariants in an Input3D payload.
/// @param input Borrowed Input3D payload; NULL is ignored.
void game3d_input_repair_state(rt_game3d_input *input);

/// @brief Synchronize an entity's physics body transform from its scene node.
/// @param entity Borrowed entity payload whose node and body may be synchronized.
/// @param force Nonzero to synchronize even when the body mode would normally skip the update.
void game3d_sync_body_from_entity_node(rt_game3d_entity *entity, int8_t force);

/// @brief Assign a SceneNode3D world position from unboxed components.
/// @param node Borrowed SceneNode3D handle.
/// @param world_pos Three-component world-space position.
void game3d_set_node_world_position(void *node, double world_pos[3]);

/// @brief Assign a SceneNode3D world orientation.
/// @param node Borrowed SceneNode3D handle.
/// @param world_quat Borrowed Quaternion handle containing the world orientation.
void game3d_set_node_world_rotation(void *node, void *world_quat);

/// @brief Shared planar character drive: integrate jump/gravity and move the wrapped
///   Character3D along an explicit XZ basis (already normalized), then sync the entity.
/// @details Used by CharacterController3D.update (camera-derived basis) and the
///   third-person controller (yaw-derived basis) so vertical-velocity state lives in
///   exactly one place.
/// @param controller Borrowed character-controller payload to advance.
/// @param input_obj Borrowed Input3D handle supplying movement and jump state.
/// @param fx Normalized forward-basis X component.
/// @param fz Normalized forward-basis Z component.
/// @param rx Normalized right-basis X component.
/// @param rz Normalized right-basis Z component.
/// @param dt Sanitized simulation delta in seconds.
void game3d_character_controller_drive(rt_game3d_character_controller *controller,
                                       void *input_obj,
                                       double fx,
                                       double fz,
                                       double rx,
                                       double rz,
                                       double dt);

/// @brief Restore all faded occluder materials and drop the fade bookkeeping array.
/// @param controller Borrowed third-person controller payload to reset.
void game3d_thirdperson_reset_fades(rt_game3d_thirdperson_controller *controller);

/// @brief Combat pass (rt_game3d_combat.c): clears one-shot health flags and event
///   buffers, ticks i-frames, then overlaps live hit volumes against hurt volumes
///   and emits HitEvent3D records. Runs after animation + scene sync each step.
/// @param world Borrowed live world payload to update.
/// @param dt Sanitized simulation delta in seconds.
void game3d_world_update_combat(rt_game3d_world *world, double dt);

/// @brief Release the world's buffered hit/damage event records (world teardown).
/// @param world Borrowed world payload whose event buffers are released.
void game3d_world_clear_combat_events(rt_game3d_world *world);

/// @brief Release an entity's hitbox array and health slot, clearing backrefs
///   first so surviving handles fail closed (entity despawn/teardown path).
/// @param entity Borrowed entity payload whose combat components are detached.
void game3d_entity_release_combat_slots(rt_game3d_entity *entity);

/// @brief Timeline pre-physics tick: advance the playhead and fire point tracks
///   (anim/audio/markers).
/// @param world Borrowed live world payload.
/// @param dt Sanitized simulation delta in seconds.
/// @return Nonzero while an active camera track should suspend the normal controller.
int game3d_world_timeline_pre(rt_game3d_world *world, double dt);

/// @brief Timeline camera application (the suspended controller's late slot).
/// @param world Borrowed live world payload.
void game3d_world_timeline_camera(rt_game3d_world *world);

/// @brief Timeline overlay pass: letterbox bars, fade quad, active subtitle.
/// @param world Borrowed live world payload.
void game3d_world_timeline_overlay(rt_game3d_world *world);

/// @brief Dialogue tick: typewriter reveal + auto-advance (scaled dt).
/// @param world Borrowed live world payload.
/// @param dt Sanitized scaled simulation delta in seconds.
void game3d_world_dialogue_tick(rt_game3d_world *world, double dt);

/// @brief Dialogue overlay pass: panel/bubble, speaker name, choices.
/// @param world Borrowed live world payload.
void game3d_world_dialogue_overlay(rt_game3d_world *world);

/// @brief Facial tick (lip sync envelope + blink + gaze), after ragdoll sync.
/// @param world Borrowed live world payload.
/// @param dt Sanitized simulation delta in seconds.
void game3d_world_facial_tick(rt_game3d_world *world, double dt);

/// @brief Advance an entity's footstep cadence and emit any due surface-aware step.
/// @param world Borrowed world providing physics and audio services.
/// @param entity Borrowed entity whose Footsteps3D component is advanced.
/// @param dt Sanitized simulation delta in seconds.
void game3d_footsteps_tick(rt_game3d_world *world, rt_game3d_entity *entity, double dt);

/// @brief Refresh an entity interactor's scored focus candidate.
/// @param world Borrowed world providing the entity and physics queries.
/// @param owner Borrowed entity whose Interactor3D component is advanced.
/// @param dt Sanitized simulation delta in seconds.
void game3d_interactor_tick(rt_game3d_world *world, rt_game3d_entity *owner, double dt);

/// @brief Advance an entity's perception and behavior-tree AI components.
/// @param world Borrowed world providing query context.
/// @param entity Borrowed entity whose AI components are advanced.
/// @param dt Sanitized simulation delta in seconds.
void game3d_ai_tick(rt_game3d_world *world, rt_game3d_entity *entity, double dt);

/// @brief Append a damage event record to the world buffer (Health3D.damage).
/// @param world Borrowed world receiving the event.
/// @param victim Borrowed damaged entity payload.
/// @param source Borrowed source entity payload, or `NULL`.
/// @param amount Sanitized damage amount applied to the victim.
/// @param tag Caller-defined damage classification.
/// @param was_lethal Nonzero when this damage transitioned the victim to dead.
void game3d_world_push_damage_event(rt_game3d_world *world,
                                    rt_game3d_entity *victim,
                                    rt_game3d_entity *source,
                                    double amount,
                                    int64_t tag,
                                    int8_t was_lethal);

/// @brief Remove a physics-body mapping from a world's dense reverse index.
/// @param world Borrowed world owning the index.
/// @param body Borrowed Physics3DBody handle to remove; `NULL` is ignored.
void game3d_world_body_index_remove(rt_game3d_world *world, void *body);

/// @brief Add or refresh the body-to-entity mapping for an entity.
/// @param world Borrowed world owning the index.
/// @param entity Borrowed entity whose valid body should be indexed.
/// @return Nonzero on success or when no body needs indexing; zero on allocation failure.
int game3d_world_body_index_add(rt_game3d_world *world, rt_game3d_entity *entity);

/// @brief Add an entity's non-empty name to a world's lookup index.
/// @param world Borrowed world owning the index.
/// @param entity Borrowed entity whose retained name should be indexed.
/// @return Nonzero on success or when no name needs indexing; zero on allocation failure.
int game3d_world_name_index_add_entity(rt_game3d_world *world, rt_game3d_entity *entity);

/// @brief Find a live spawned entity by name through the world's lookup index.
/// @param world Borrowed world containing the name index.
/// @param name Null-terminated entity name to find.
/// @return Borrowed matching entity payload, or `NULL` when no live match exists.
rt_game3d_entity *game3d_world_name_index_find(rt_game3d_world *world, const char *name);

/// @brief Test whether an integer is a valid Game3D collision-layer number.
/// @param layer Candidate one-based layer number.
/// @return Nonzero when @p layer is within the supported layer range.
int8_t game3d_valid_layer(int64_t layer);

/// @brief Allocate a LayerMask handle from raw mask bits.
/// @param bits Collision-layer bitfield to store.
/// @return New GC-managed LayerMask handle, or `NULL` on allocation failure.
void *game3d_layermask_new_bits(int64_t bits);

/// @brief Instantiate a physics body from a reusable BodyDef payload.
/// @param def Borrowed body definition containing the type, shape, transform, and material data.
/// @return New GC-managed Physics3DBody handle, or `NULL` after invalid input or allocation
/// failure.
void *game3d_body_def_create_body(rt_game3d_body_def *def);

/// @brief Ensure the tracked-audio-source array can hold a requested count.
/// @param audio Borrowed Sound3D payload whose array may grow.
/// @param needed Minimum source capacity required.
/// @return Nonzero on success; zero for invalid input, overflow, or allocation failure.
int game3d_audio_reserve_sources(rt_game3d_audio *audio, int32_t needed);

/// @brief Remove stale entries and restore invariants in a Sound3D source registry.
/// @param audio Borrowed Sound3D payload to repair.
void game3d_audio_repair_sources(rt_game3d_audio *audio);

/// @brief Retain and register an audio source for later pruning and rebasing.
/// @param audio Borrowed Sound3D payload receiving the source.
/// @param source Borrowed AudioSource3D handle; invalid or duplicate sources are ignored.
void game3d_audio_track_source(rt_game3d_audio *audio, void *source);

/// @brief Release finished or invalid sources from a Sound3D registry.
/// @param audio Borrowed Sound3D payload to prune.
void game3d_audio_prune_sources(rt_game3d_audio *audio);

/// @brief Shift all tracked positional sources by a floating-origin delta.
/// @param audio Borrowed Sound3D payload.
/// @param delta Three-component world-origin displacement to subtract.
void game3d_audio_rebase_origin(rt_game3d_audio *audio, const double delta[3]);

/// @brief Update listener-relative audio immersion effects for a world.
/// @param world Borrowed live world payload.
/// @param dt Sanitized simulation delta in seconds.
void game3d_audio_immersion_tick(struct rt_game3d_world *world, double dt);

/// @brief Advance every cloth component registered with a world.
/// @param world Borrowed live world payload.
/// @param dt Sanitized simulation delta in seconds.
void game3d_cloth_tick(struct rt_game3d_world *world, double dt);

/// @brief Capture due persistent entity-state deltas for a world.
/// @param world Borrowed live world payload.
void game3d_persistence_tick(struct rt_game3d_world *world);

/// @brief Snapshot a persistent entity immediately before it despawns.
/// @param world Borrowed world owning the persistence records.
/// @param entity Borrowed entity about to leave the world.
void game3d_persistence_on_despawn(struct rt_game3d_world *world, struct rt_game3d_entity *entity);

/// @brief Release every persistent-state record owned by a world.
/// @param world Borrowed world payload being reset or destroyed.
void game3d_persistence_release(struct rt_game3d_world *world);

/// @brief Append a loaded-cell name to a stream's one-shot event queue.
/// @param stream Borrowed WorldStream3D payload receiving the event.
/// @param cell_name Borrowed runtime string naming the loaded cell.
void game3d_stream_push_loaded_event(struct rt_game3d_world_stream *stream, rt_string cell_name);

/// @brief Release persistence data owned by a world-stream payload.
/// @param stream Borrowed WorldStream3D payload being reset or destroyed.
void game3d_stream_persistence_release(struct rt_game3d_world_stream *stream);

/// @brief Record a frame hitch and its subsystem telemetry when a threshold is exceeded.
/// @param world Borrowed world whose fixed-capacity hitch ring receives the sample.
/// @param step_wall_ms Measured wall-clock duration of the simulation step in milliseconds.
void game3d_world_note_hitches(struct rt_game3d_world *world, double step_wall_ms);

/// @brief Clamp a signed 64-bit integer to an inclusive interval.
/// @param value Value to bound.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @return @p value bounded to `[`@p lo`,` @p hi`]`.
int64_t game3d_clamp_i64(int64_t value, int64_t lo, int64_t hi);

/// @brief Allocate a Sound3D service bound to a camera listener.
/// @param camera Borrowed Camera3D handle to retain as the listener transform.
/// @return New GC-managed Sound3D handle, or `NULL` on allocation failure.
void *game3d_audio_new(void *camera);

/// @brief Release one effect item's retained object and reset its bookkeeping.
/// @param item Effect registry slot to clear; `NULL` is ignored.
void game3d_effect_release_item(rt_game3d_effect_item *item);

/// @brief Remove stale effects and restore EffectRegistry3D array invariants.
/// @param effects Borrowed effect registry payload to repair.
void game3d_effects_repair(rt_game3d_effects *effects);

/// @brief Ensure an effect registry can hold a requested item count.
/// @param effects Borrowed effect registry payload whose array may grow.
/// @param needed Minimum item capacity required.
/// @return Nonzero on success; zero for invalid input, overflow, or allocation failure.
int game3d_effects_reserve(rt_game3d_effects *effects, int32_t needed);

/// @brief Validate and copy a Vec3 handle into an unboxed destination.
/// @param vec Borrowed Vec3 handle.
/// @param[out] out Required three-component destination.
/// @param method Diagnostic message used when validation fails.
/// @return Nonzero on success; zero after invalid input or a recorded trap.
int8_t game3d_read_vec3(void *vec, double *out, const char *method);

/// @brief Allocate an EffectRegistry3D bound to a render canvas.
/// @param canvas Borrowed Canvas3D handle retained by the registry.
/// @param quality Requested effect-quality preset.
/// @return New GC-managed EffectRegistry3D handle, or `NULL` on allocation failure.
void *game3d_effects_new(void *canvas, int64_t quality);

/// @brief Remove an entity from its parent's child array without destroying it.
/// @param child Borrowed entity payload to detach; root entities are unchanged.
void game3d_entity_detach_from_parent(rt_game3d_entity *child);

/// @brief Find a child's slot in a parent's dense child array.
/// @param parent Borrowed parent entity payload.
/// @param child Borrowed child entity payload to locate.
/// @return Zero-based slot index, or `-1` when the relationship is absent or invalid.
int32_t game3d_entity_find_child_index(rt_game3d_entity *parent, rt_game3d_entity *child);

/// @brief Ensure an entity child array can hold a requested count.
/// @param entity Borrowed parent entity whose child storage may grow.
/// @param need Minimum child capacity required.
/// @return Nonzero on success; zero for invalid input, overflow, or allocation failure.
int game3d_entity_grow_children(rt_game3d_entity *entity, int32_t need);

/// @brief Test whether an entity appears in another entity's parent chain.
/// @param entity Borrowed entity at which to begin the upward traversal.
/// @param ancestor Borrowed entity to search for.
/// @return Nonzero when @p ancestor owns @p entity transitively; otherwise zero.
int game3d_entity_has_ancestor(rt_game3d_entity *entity, rt_game3d_entity *ancestor);

/// @brief Resolve a physics body to its live owning entity.
/// @param world Borrowed world containing the body reverse index.
/// @param body Borrowed Physics3DBody handle to resolve.
/// @return Borrowed live entity payload, or `NULL` when no valid mapping exists.
rt_game3d_entity *game3d_world_find_entity_by_body(rt_game3d_world *world, void *body);

/// @brief Spawn an entity subtree into a world and optionally attach its root to the scene.
/// @param world Borrowed destination world payload.
/// @param entity Borrowed root entity payload.
/// @param attach_to_scene Nonzero to attach the root node to the world's scene.
/// @param[in,out] next_id Monotonic identifier source updated for each newly spawned entity.
/// @return Nonzero on success; zero after validation, capacity, or allocation failure.
int game3d_world_spawn_entity_tree(rt_game3d_world *world,
                                   rt_game3d_entity *entity,
                                   int attach_to_scene,
                                   int64_t *next_id);

/// @brief Query whether a keyboard key is down in the snapshot or live input state.
/// @param input Borrowed Input3D payload.
/// @param key Runtime key code.
/// @return Nonzero when the key is currently held; otherwise zero.
int8_t game3d_input_key_down(const rt_game3d_input *input, int64_t key);

/// @brief Query the one-shot pressed edge for a keyboard key.
/// @param input Borrowed Input3D payload.
/// @param key Runtime key code.
/// @return Nonzero when the key transitioned down in the observed frame.
int8_t game3d_input_key_pressed(const rt_game3d_input *input, int64_t key);

/// @brief Query the one-shot released edge for a keyboard key.
/// @param input Borrowed Input3D payload.
/// @param key Runtime key code.
/// @return Nonzero when the key transitioned up in the observed frame.
int8_t game3d_input_key_released(const rt_game3d_input *input, int64_t key);

/// @brief Query whether a mouse button is down in the snapshot or live input state.
/// @param input Borrowed Input3D payload.
/// @param button Runtime mouse-button code.
/// @return Nonzero when the button is currently held; otherwise zero.
int8_t game3d_input_mouse_down(const rt_game3d_input *input, int64_t button);

/// @brief Query the pressed edge for a mouse button from the coherent snapshot.
/// @param input Borrowed Input3D payload.
/// @param button Runtime mouse-button code.
/// @return Nonzero when the button transitioned down in the captured frame.
int8_t game3d_input_mouse_pressed_snapshot(const rt_game3d_input *input, int64_t button);

/// @brief Normalize a three-component axis vector in place.
/// @param[in,out] x X component to sanitize and normalize.
/// @param[in,out] y Y component to sanitize and normalize.
/// @param[in,out] z Z component to sanitize and normalize.
void game3d_normalize_axis3(double *x, double *y, double *z);

/// @brief Replace the world's retained post-processing stack.
/// @param world Borrowed world payload whose post-processing slot is updated.
/// @param postfx Borrowed PostProcess3D handle to retain, or `NULL` to clear the slot.
void game3d_world_assign_postfx(rt_game3d_world *world, void *postfx);

/// @brief Install a light into one of the world's retained environment slots.
/// @param world Borrowed world payload whose environment changes.
/// @param slot Environment-light slot selector.
/// @param light Borrowed light handle to retain, or `NULL` to clear the slot.
void game3d_world_install_light(rt_game3d_world *world, int64_t slot, void *light);

/// @brief Sanitize and store the world's clear color.
/// @param world Borrowed world payload to update.
/// @param r Red channel value.
/// @param g Green channel value.
/// @param b Blue channel value.
void game3d_world_set_clear_color(rt_game3d_world *world, double r, double g, double b);
