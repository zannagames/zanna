//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_bodydef.c
// Purpose: BodyDef3D builder for the Zanna.Game3D layer — shape/material/layer
//   setters and getters plus the BodyDef -> Physics3D body factory. Split out of
//   rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Key invariants:
//   - BodyDef is a plain value object; create_body materializes a physics body.
// Ownership/Lifetime:
//   - GC-managed BodyDef handle; no retained references.
// Links: rt_game3d_internal.h, rt_physics3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the Game3D BodyDef value builder and physics-body factory.
/// @details BodyDef normalizes shape dimensions, material properties, simulation modes, and
///   collision filtering without retaining external objects. The factory materializes a concrete
///   Physics3D body only after the complete definition has been configured.

#include "rt_animcontroller3d.h"
#include "rt_asset.h"
#include "rt_audio.h"
#include "rt_box.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_collider3d.h"
#include "rt_decal3d.h"
#include "rt_g3d_commit_queue.h"
#include "rt_game3d.h"
#include "rt_game3d_internal.h"
#include "rt_gltf.h"
#include "rt_graphics3d_ids.h"
#include "rt_input.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_mat4.h"
#include "rt_model3d.h"
#include "rt_navmesh3d.h"
#include "rt_object.h"
#include "rt_parallel.h"
#include "rt_particles3d.h"
#include "rt_physics3d.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_postfx3d.h"
#include "rt_quat.h"
#include "rt_scene3d.h"
#include "rt_scene3d_internal.h"
#include "rt_seq.h"
#include "rt_sound3d.h"
#include "rt_soundlistener3d.h"
#include "rt_soundsource3d.h"
#include "rt_string.h"
#include "rt_terrain3d.h"
#include "rt_textureasset3d.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
#include "rt_vec2.h"
#include "rt_vec3.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_GAME3D_BODYDEF_DIM_MAX 1000000.0
#define RT_GAME3D_BODYDEF_MASS_MAX 1000000.0
#define RT_GAME3D_BODYDEF_FRICTION_MAX 1000.0

/// @brief Reset a BodyDef to library defaults: a 0.5-half-extent dynamic box, mass 1,
///   friction 0.5, restitution 0.3, on the DYNAMIC layer, colliding with everything.
/// @param def BodyDef storage to initialize; NULL is ignored.
static void game3d_body_def_defaults(rt_game3d_body_def *def) {
    if (!def)
        return;
    memset(def, 0, sizeof(*def));
    def->shape = RT_GAME3D_BODY_SHAPE_BOX;
    def->half_extents[0] = 0.5;
    def->half_extents[1] = 0.5;
    def->half_extents[2] = 0.5;
    def->radius = 0.5;
    def->height = 1.0;
    def->mass = 1.0;
    def->friction = 0.5;
    def->restitution = 0.3;
    def->layer = RT_GAME3D_LAYER_DYNAMIC;
    def->mask_bits = ~(int64_t)0;
    def->sync_mode = RT_GAME3D_SYNC_NODE_FROM_BODY;
}

/// @brief Allocate a BodyDef handle seeded with defaults; traps `method` on OOM.
/// @param method Trap message used when allocation fails.
/// @return A new default BodyDef, or NULL on allocation failure.
static void *game3d_body_def_alloc(const char *method) {
    rt_game3d_body_def *def =
        (rt_game3d_body_def *)rt_obj_new_i64(RT_G3D_GAME3D_BODYDEF_CLASS_ID, (int64_t)sizeof(*def));
    if (!def) {
        rt_trap(method ? method : "Game3D.BodyDef: allocation failed");
        return NULL;
    }
    game3d_body_def_defaults(def);
    return def;
}

/// @brief Return `sync_mode` if it is a known RT_GAME3D_SYNC_* value, else default to
///   NODE_FROM_BODY.
/// @param sync_mode Candidate body/node synchronization mode.
/// @return The validated mode, or RT_GAME3D_SYNC_NODE_FROM_BODY as the fallback.
static int64_t game3d_valid_sync_or_default(int64_t sync_mode) {
    switch (sync_mode) {
        case RT_GAME3D_SYNC_NODE_FROM_BODY:
        case RT_GAME3D_SYNC_BODY_FROM_NODE:
        case RT_GAME3D_SYNC_NODE_FROM_ANIM_ROOT_MOTION:
        case RT_GAME3D_SYNC_TWO_WAY_KINEMATIC:
            return sync_mode;
        default:
            return RT_GAME3D_SYNC_NODE_FROM_BODY;
    }
}

/// @brief Return `value` if finite and strictly positive, else `fallback`. For
///   collider extents/radii/heights that must stay positive.
/// @param value Candidate positive dimension.
/// @param fallback Value used when @p value is non-finite or non-positive.
/// @return The selected dimension, clamped to RT_GAME3D_BODYDEF_DIM_MAX.
static double game3d_bodydef_extent_or(double value, double fallback) {
    value = game3d_finite_or(value, fallback);
    if (value <= 0.0)
        return fallback;
    if (value > RT_GAME3D_BODYDEF_DIM_MAX)
        return RT_GAME3D_BODYDEF_DIM_MAX;
    return value;
}

/// @brief Build a dynamic box BodyDef; a (near-)zero mass yields a static body. See header.
/// @param half_x Positive half-extent along X.
/// @param half_y Positive half-extent along Y.
/// @param half_z Positive half-extent along Z.
/// @param mass Non-negative mass in kilograms.
/// @return A new box BodyDef, or NULL when allocation fails.
void *rt_game3d_body_def_box(double half_x, double half_y, double half_z, double mass) {
    rt_game3d_body_def *def =
        (rt_game3d_body_def *)game3d_body_def_alloc("Game3D.BodyDef.Box: allocation failed");
    if (!def)
        return NULL;
    def->shape = RT_GAME3D_BODY_SHAPE_BOX;
    def->half_extents[0] = game3d_bodydef_extent_or(half_x, 0.5);
    def->half_extents[1] = game3d_bodydef_extent_or(half_y, 0.5);
    def->half_extents[2] = game3d_bodydef_extent_or(half_z, 0.5);
    def->mass = game3d_nonnegative_clamped_or(mass, 1.0, RT_GAME3D_BODYDEF_MASS_MAX);
    def->is_static = def->mass <= 1e-12 ? 1 : 0;
    return def;
}

/// @brief Build a dynamic sphere BodyDef; a (near-)zero mass yields a static body. See header.
/// @param radius Positive sphere radius.
/// @param mass Non-negative mass in kilograms.
/// @return A new sphere BodyDef, or NULL when allocation fails.
void *rt_game3d_body_def_sphere(double radius, double mass) {
    rt_game3d_body_def *def =
        (rt_game3d_body_def *)game3d_body_def_alloc("Game3D.BodyDef.Sphere: allocation failed");
    if (!def)
        return NULL;
    def->shape = RT_GAME3D_BODY_SHAPE_SPHERE;
    def->radius = game3d_bodydef_extent_or(radius, 0.5);
    def->mass = game3d_nonnegative_clamped_or(mass, 1.0, RT_GAME3D_BODYDEF_MASS_MAX);
    def->is_static = def->mass <= 1e-12 ? 1 : 0;
    return def;
}

/// @brief Build a dynamic capsule BodyDef (height clamped to ≥ 2·radius); zero mass
///   yields a static body. See header.
/// @param radius Positive capsule radius.
/// @param height Positive full capsule height.
/// @param mass Non-negative mass in kilograms.
/// @return A new capsule BodyDef, or NULL when allocation fails.
void *rt_game3d_body_def_capsule(double radius, double height, double mass) {
    rt_game3d_body_def *def =
        (rt_game3d_body_def *)game3d_body_def_alloc("Game3D.BodyDef.Capsule: allocation failed");
    if (!def)
        return NULL;
    def->shape = RT_GAME3D_BODY_SHAPE_CAPSULE;
    def->radius = game3d_bodydef_extent_or(radius, 0.25);
    def->height = game3d_bodydef_extent_or(height, def->radius * 2.0);
    if (def->height < def->radius * 2.0)
        def->height = def->radius * 2.0;
    def->mass = game3d_nonnegative_clamped_or(mass, 1.0, RT_GAME3D_BODYDEF_MASS_MAX);
    def->is_static = def->mass <= 1e-12 ? 1 : 0;
    return def;
}

/// @brief Build a static box on the WORLD layer (mass 0); see header.
/// @param half_x Positive half-extent along X.
/// @param half_y Positive half-extent along Y.
/// @param half_z Positive half-extent along Z.
/// @return A new static world-layer box BodyDef, or NULL when allocation fails.
void *rt_game3d_body_def_static_box(double half_x, double half_y, double half_z) {
    rt_game3d_body_def *def =
        (rt_game3d_body_def *)rt_game3d_body_def_box(half_x, half_y, half_z, 0.0);
    if (!def)
        return NULL;
    def->is_static = 1;
    def->layer = RT_GAME3D_LAYER_WORLD;
    def->has_layer = 1;
    return def;
}

/// @brief Build a thin static floor box of the given footprint size; see header.
/// @param size Positive full width and depth of the floor footprint.
/// @return A new static world-layer floor BodyDef, or NULL when allocation fails.
void *rt_game3d_body_def_static_plane(double size) {
    double half = game3d_bodydef_extent_or(size, 1.0) * 0.5;
    rt_game3d_body_def *def = (rt_game3d_body_def *)rt_game3d_body_def_static_box(half, 0.05, half);
    return def;
}

/// @brief Get the body shape kind (defaults to BOX if the handle is invalid).
/// @param obj BodyDef to query.
/// @return A validated RT_GAME3D_BODY_SHAPE_* value.
int64_t rt_game3d_body_def_get_shape(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_shape: invalid BodyDef");
    if (!def)
        return RT_GAME3D_BODY_SHAPE_BOX;
    switch (def->shape) {
        case RT_GAME3D_BODY_SHAPE_BOX:
        case RT_GAME3D_BODY_SHAPE_SPHERE:
        case RT_GAME3D_BODY_SHAPE_CAPSULE:
            return def->shape;
        default:
            return RT_GAME3D_BODY_SHAPE_BOX;
    }
}

/// @brief Set the body shape kind; traps on an unknown BodyShape value.
/// @param obj BodyDef to configure.
/// @param shape One of the RT_GAME3D_BODY_SHAPE_* values.
void rt_game3d_body_def_set_shape(void *obj, int64_t shape) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_shape: invalid BodyDef");
    if (!def)
        return;
    switch (shape) {
        case RT_GAME3D_BODY_SHAPE_BOX:
        case RT_GAME3D_BODY_SHAPE_SPHERE:
        case RT_GAME3D_BODY_SHAPE_CAPSULE:
            def->shape = shape;
            break;
        default:
            rt_trap("Game3D.BodyDef.set_shape: invalid BodyShape");
            break;
    }
}

/// @brief Get the body mass in kg (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return The sanitized non-negative mass in kilograms, or 0 when invalid.
double rt_game3d_body_def_get_mass(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_mass: invalid BodyDef");
    return def ? game3d_nonnegative_clamped_or(def->mass, 0.0, RT_GAME3D_BODYDEF_MASS_MAX) : 0.0;
}

/// @brief Set the body mass; zero mass means static, positive mass means simulated.
/// @param obj BodyDef to configure.
/// @param mass Requested non-negative mass in kilograms.
void rt_game3d_body_def_set_mass(void *obj, double mass) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_mass: invalid BodyDef");
    if (def) {
        def->mass = game3d_nonnegative_clamped_or(mass, def->mass, RT_GAME3D_BODYDEF_MASS_MAX);
        if (def->mass <= 1e-12) {
            def->is_static = 1;
            def->is_kinematic = 0;
        } else {
            def->is_static = 0;
        }
    }
}

/// @brief Get the friction coefficient (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return The sanitized non-negative friction coefficient, or 0 when invalid.
double rt_game3d_body_def_get_friction(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_friction: invalid BodyDef");
    return def ? game3d_nonnegative_clamped_or(def->friction, 0.0, RT_GAME3D_BODYDEF_FRICTION_MAX)
               : 0.0;
}

/// @brief Set the friction coefficient (negatives keep the prior value).
/// @param obj BodyDef to configure.
/// @param friction Requested non-negative friction coefficient.
void rt_game3d_body_def_set_friction(void *obj, double friction) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_friction: invalid BodyDef");
    if (def)
        def->friction =
            game3d_nonnegative_clamped_or(friction, def->friction, RT_GAME3D_BODYDEF_FRICTION_MAX);
}

/// @brief Get the restitution coefficient (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return The restitution coefficient in the range 0 through 1, or 0 when invalid.
double rt_game3d_body_def_get_restitution(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_restitution: invalid BodyDef");
    return def ? game3d_clamp(def->restitution, 0.0, 1.0) : 0.0;
}

/// @brief Set the restitution coefficient, clamped to [0, 1].
/// @param obj BodyDef to configure.
/// @param restitution Requested coefficient of restitution.
void rt_game3d_body_def_set_restitution(void *obj, double restitution) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_restitution: invalid BodyDef");
    if (def)
        def->restitution = game3d_clamp(restitution, 0.0, 1.0);
}

/// @brief True if the body is static (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return 1 when configured static, or 0 when dynamic or invalid.
int8_t rt_game3d_body_def_get_static(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_isStatic: invalid BodyDef");
    return def && def->is_static ? 1 : 0;
}

/// @brief Mark the body static/dynamic; static bodies have zero mass, and returning to
///   dynamic restores a usable default mass if no explicit mass is present.
/// @param obj BodyDef to configure.
/// @param is_static Non-zero for a mass-zero static body; zero for a simulated body.
void rt_game3d_body_def_set_static(void *obj, int8_t is_static) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_isStatic: invalid BodyDef");
    if (def) {
        def->is_static = is_static ? 1 : 0;
        if (def->is_static) {
            def->mass = 0.0;
            def->is_kinematic = 0;
        } else if (def->mass <= 1e-12) {
            def->mass = 1.0;
        }
    }
}

/// @brief True if the body is kinematic (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return 1 when configured kinematic, or 0 when simulated or invalid.
int8_t rt_game3d_body_def_get_kinematic(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_isKinematic: invalid BodyDef");
    return def && def->is_kinematic ? 1 : 0;
}

/// @brief Mark the body kinematic/simulated; going kinematic clears static and ensures mass.
/// @param obj BodyDef to configure.
/// @param is_kinematic Non-zero for a kinematic body; zero for normal simulation.
void rt_game3d_body_def_set_kinematic(void *obj, int8_t is_kinematic) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_isKinematic: invalid BodyDef");
    if (def) {
        def->is_kinematic = is_kinematic ? 1 : 0;
        if (def->is_kinematic) {
            def->is_static = 0;
            if (def->mass <= 1e-12)
                def->mass = 1.0;
        }
    }
}

/// @brief True if the body is a trigger (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return 1 when configured as a trigger volume, or 0 otherwise.
int8_t rt_game3d_body_def_get_trigger(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_isTrigger: invalid BodyDef");
    return def && def->is_trigger ? 1 : 0;
}

/// @brief Mark the body as a trigger volume or a solid collider.
/// @param obj BodyDef to configure.
/// @param is_trigger Non-zero for a trigger volume; zero for a solid collider.
void rt_game3d_body_def_set_trigger(void *obj, int8_t is_trigger) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_isTrigger: invalid BodyDef");
    if (def)
        def->is_trigger = is_trigger ? 1 : 0;
}

/// @brief True if continuous collision detection is enabled (0 on invalid handle).
/// @param obj BodyDef to query.
/// @return 1 when CCD is enabled, or 0 when disabled or invalid.
int8_t rt_game3d_body_def_get_use_ccd(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_useCCD: invalid BodyDef");
    return def && def->use_ccd ? 1 : 0;
}

/// @brief Enable or disable continuous collision detection.
/// @param obj BodyDef to configure.
/// @param use_ccd Non-zero to enable CCD; zero to disable it.
void rt_game3d_body_def_set_use_ccd(void *obj, int8_t use_ccd) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.set_useCCD: invalid BodyDef");
    if (def)
        def->use_ccd = use_ccd ? 1 : 0;
}

/// @brief Get the body's collision layer (defaults to DYNAMIC on invalid handle).
/// @param obj BodyDef to query.
/// @return A valid single-bit collision layer.
int64_t rt_game3d_body_def_get_layer(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_layer: invalid BodyDef");
    return def && game3d_valid_layer(def->layer) ? def->layer : RT_GAME3D_LAYER_DYNAMIC;
}

/// @brief Property setter for the collision layer (delegates to withLayer).
/// @param obj BodyDef to configure.
/// @param layer Positive single-bit collision layer.
void rt_game3d_body_def_set_layer_prop(void *obj, int64_t layer) {
    (void)rt_game3d_body_def_with_layer(obj, layer);
}

/// @brief Get a fresh LayerMask handle reflecting the body's collision mask bits.
/// @param obj BodyDef to query.
/// @return A new LayerMask containing the configured bits, or NULL when invalid.
void *rt_game3d_body_def_get_mask(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_mask: invalid BodyDef");
    return def ? game3d_layermask_new_bits(def->mask_bits) : NULL;
}

/// @brief Property setter for the collision mask (delegates to withMask).
/// @param obj BodyDef to configure.
/// @param mask LayerMask whose bits are copied into the definition.
void rt_game3d_body_def_set_mask_prop(void *obj, void *mask) {
    (void)rt_game3d_body_def_with_mask(obj, mask);
}

/// @brief Get the body/node sync mode (defaults to NODE_FROM_BODY on invalid handle).
/// @param obj BodyDef to query.
/// @return A validated RT_GAME3D_SYNC_* mode.
int64_t rt_game3d_body_def_get_sync_mode(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.get_syncMode: invalid BodyDef");
    return def ? game3d_valid_sync_or_default(def->sync_mode) : RT_GAME3D_SYNC_NODE_FROM_BODY;
}

/// @brief Property setter for the sync mode (delegates to withSync).
/// @param obj BodyDef to configure.
/// @param sync_mode Candidate RT_GAME3D_SYNC_* value.
void rt_game3d_body_def_set_sync_mode_prop(void *obj, int64_t sync_mode) {
    (void)rt_game3d_body_def_with_sync(obj, sync_mode);
}

/// @brief Fluent: set the collision layer (must be a single bit) and return the def.
/// @param obj BodyDef to configure.
/// @param layer Positive single-bit collision layer.
/// @return @p obj for fluent chaining.
void *rt_game3d_body_def_with_layer(void *obj, int64_t layer) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.withLayer: invalid BodyDef");
    if (!game3d_valid_layer(layer)) {
        rt_trap("Game3D.BodyDef.withLayer: layer must be a single positive bit");
        return obj;
    }
    if (def) {
        def->layer = layer;
        def->has_layer = 1;
    }
    return obj;
}

/// @brief Fluent: copy a LayerMask's bits into the def's collision mask and return it.
/// @param obj BodyDef to configure.
/// @param mask_obj LayerMask whose bits are copied.
/// @return @p obj for fluent chaining.
void *rt_game3d_body_def_with_mask(void *obj, void *mask_obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.withMask: invalid BodyDef");
    rt_game3d_layermask *mask =
        game3d_layermask_checked(mask_obj, "Game3D.BodyDef.withMask: invalid mask");
    if (def && mask) {
        def->mask_bits = mask->bits;
        def->has_mask = 1;
    }
    return obj;
}

/// @brief Fluent: mark the def as a trigger and return it.
/// @param obj BodyDef to configure.
/// @return @p obj for fluent chaining.
void *rt_game3d_body_def_as_trigger(void *obj) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.asTrigger: invalid BodyDef");
    if (def)
        def->is_trigger = 1;
    return obj;
}

/// @brief Fluent: set the (validated) sync mode and return the def.
/// @param obj BodyDef to configure.
/// @param sync_mode Candidate RT_GAME3D_SYNC_* value.
/// @return @p obj for fluent chaining.
void *rt_game3d_body_def_with_sync(void *obj, int64_t sync_mode) {
    rt_game3d_body_def *def =
        game3d_body_def_checked(obj, "Game3D.BodyDef.withSync: invalid BodyDef");
    if (def)
        def->sync_mode = game3d_valid_sync_or_default(sync_mode);
    return obj;
}

/// @brief Instantiate the concrete Physics3D body matching a BodyDef's shape and mass
///   (static bodies are created with mass 0). Returns NULL for a NULL def.
/// @param def BodyDef whose normalized values configure the new physics body.
/// @return A new Body3D of the selected shape, or NULL when creation fails.
void *game3d_body_def_create_body(rt_game3d_body_def *def) {
    void *body = NULL;
    if (!def)
        return NULL;
    double mass = game3d_nonnegative_clamped_or(def->mass, 1.0, RT_GAME3D_BODYDEF_MASS_MAX);
    double radius = game3d_bodydef_extent_or(def->radius, 0.5);
    double height = game3d_bodydef_extent_or(def->height, radius * 2.0);
    if (height < radius * 2.0)
        height = radius * 2.0;
    double half_x = game3d_bodydef_extent_or(def->half_extents[0], 0.5);
    double half_y = game3d_bodydef_extent_or(def->half_extents[1], 0.5);
    double half_z = game3d_bodydef_extent_or(def->half_extents[2], 0.5);
    double body_mass = def->is_static ? 0.0 : mass;
    switch (def->shape) {
        case RT_GAME3D_BODY_SHAPE_SPHERE:
            body = rt_body3d_new_sphere(radius, body_mass);
            break;
        case RT_GAME3D_BODY_SHAPE_CAPSULE:
            body = rt_body3d_new_capsule(radius, height, body_mass);
            break;
        case RT_GAME3D_BODY_SHAPE_BOX:
        default:
            body = rt_body3d_new_aabb(half_x, half_y, half_z, body_mass);
            break;
    }
    if (!body)
        return NULL;
    rt_body3d_set_friction(
        body, game3d_nonnegative_clamped_or(def->friction, 0.5, RT_GAME3D_BODYDEF_FRICTION_MAX));
    rt_body3d_set_restitution(body, game3d_clamp(def->restitution, 0.0, 1.0));
    if (def->is_static)
        rt_body3d_set_static(body, 1);
    else if (def->is_kinematic)
        rt_body3d_set_kinematic(body, 1);
    rt_body3d_set_trigger(body, def->is_trigger);
    rt_body3d_set_use_ccd(body, def->use_ccd);
    rt_body3d_set_collision_layer(
        body, game3d_valid_layer(def->layer) ? def->layer : RT_GAME3D_LAYER_DYNAMIC);
    rt_body3d_set_collision_mask(body, def->mask_bits);
    return body;
}
