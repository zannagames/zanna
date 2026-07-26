//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_effects.c
// Purpose: EffectRegistry3D + Effects3D spawners for the Zanna.Game3D layer —
//   particle/decal item management and explosion/sparks/dust/smoke/impact helpers.
//   Split out of rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Links: rt_game3d_internal.h, rt_particles3d.h, rt_decal3d.h, rt_postfx3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the Game3D effect registry and stock transient-effect spawners.
/// @details EffectRegistry3D retains particle systems and decals, advances them,
///          and removes expired or stale entries without preserving order. It
///          also owns the quality-scaled PostFx3D stack installed on the world
///          canvas. Effects3D helper functions construct deterministic presets
///          and register them with the target world's registry.

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

/// @brief GC finalizer for the effect registry: tear down every live item, free the item array,
///   and release the post-FX stack.
/// @param obj EffectRegistry3D storage being finalized; NULL is ignored.
static void game3d_effects_finalize(void *obj) {
    rt_game3d_effects *effects = (rt_game3d_effects *)obj;
    if (!effects)
        return;
    game3d_effects_repair(effects);
    for (int32_t i = 0; i < effects->count; ++i)
        game3d_effect_release_item(&effects->items[i]);
    free(effects->items);
    effects->items = NULL;
    effects->count = 0;
    effects->capacity = 0;
    game3d_release_ref(&effects->postfx);
}

/// @brief Allocate an effect registry, building a quality-scaled post-FX stack and
///   wiring it into the canvas; traps on OOM.
/// @param canvas Canvas3D that receives the newly created PostFx3D stack; may be NULL.
/// @param quality Quality preset forwarded to the PostFx3D constructor.
/// @return A newly allocated EffectRegistry3D, or NULL on allocation failure.
void *game3d_effects_new(void *canvas, int64_t quality) {
    rt_game3d_effects *effects = (rt_game3d_effects *)rt_obj_new_i64(RT_G3D_GAME3D_EFFECTS_CLASS_ID,
                                                                     (int64_t)sizeof(*effects));
    if (!effects) {
        rt_trap("Game3D.EffectRegistry3D.New: allocation failed");
        return NULL;
    }
    memset(effects, 0, sizeof(*effects));
    rt_obj_set_finalizer(effects, game3d_effects_finalize);
    effects->postfx = rt_postfx3d_new_quality(canvas, quality);
    if (effects->postfx && canvas)
        rt_canvas3d_set_post_fx(canvas, effects->postfx);
    return effects;
}

/// @brief Get the post-FX stack owned by this registry (NULL if invalid).
/// @param obj EffectRegistry3D runtime handle.
/// @return The validated owned PostFx3D pointer, or NULL.
void *rt_game3d_effects_get_postfx(void *obj) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.get_PostFx: invalid effects");
    return effects ? rt_g3d_checked_or_null(effects->postfx, RT_G3D_POSTFX3D_CLASS_ID) : NULL;
}

/// @brief Count all live effect items (particles + decals).
/// @param obj EffectRegistry3D runtime handle.
/// @return The repaired number of retained items, or zero when invalid.
int64_t rt_game3d_effects_get_count(void *obj) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.get_count: invalid effects");
    if (!effects)
        return 0;
    game3d_effects_repair(effects);
    return effects->count;
}

/// @brief Count live particle-system items.
/// @param obj EffectRegistry3D runtime handle.
/// @return The number of repaired entries tagged as Particles3D, or zero when invalid.
int64_t rt_game3d_effects_get_particles_count(void *obj) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.get_particlesCount: invalid effects");
    int64_t count = 0;
    if (!effects)
        return 0;
    game3d_effects_repair(effects);
    for (int32_t i = 0; i < effects->count; ++i) {
        if (effects->items[i].type == RT_GAME3D_EFFECT_PARTICLES)
            count++;
    }
    return count;
}

/// @brief Count live decal items.
/// @param obj EffectRegistry3D runtime handle.
/// @return The number of repaired entries tagged as Decal3D, or zero when invalid.
int64_t rt_game3d_effects_get_decal_count(void *obj) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.get_decalCount: invalid effects");
    int64_t count = 0;
    if (!effects)
        return 0;
    game3d_effects_repair(effects);
    for (int32_t i = 0; i < effects->count; ++i) {
        if (effects->items[i].type == RT_GAME3D_EFFECT_DECAL)
            count++;
    }
    return count;
}

/// @brief Register and retain a Particles3D with an auto-expire lifetime (≤0 means
///   never expire); returns the particles. Traps on a non-Particles3D handle.
/// @param obj EffectRegistry3D runtime handle.
/// @param particles Particles3D object to retain.
/// @param lifetime Positive registry lifetime in seconds; non-finite or
///                 non-positive values disable registry-driven expiry.
/// @return @p particles on success, or NULL after validation, capacity, or
///         allocation failure.
void *rt_game3d_effects_add_particles(void *obj, void *particles, double lifetime) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.addParticles: invalid effects");
    if (!effects)
        return NULL;
    if (!rt_g3d_has_class(particles, RT_G3D_PARTICLES3D_CLASS_ID)) {
        rt_trap("Game3D.EffectRegistry3D.addParticles: expected Particles3D");
        return NULL;
    }
    game3d_effects_repair(effects);
    if (effects->count == INT32_MAX) {
        rt_trap("Game3D.EffectRegistry3D: too many effects");
        return NULL;
    }
    if (!game3d_effects_reserve(effects, effects->count + 1))
        return NULL;
    rt_game3d_effect_item *item = &effects->items[effects->count++];
    memset(item, 0, sizeof(*item));
    item->type = RT_GAME3D_EFFECT_PARTICLES;
    item->object = particles;
    item->lifetime = (isfinite(lifetime) && lifetime > 0.0)
                         ? game3d_positive_clamped_or(lifetime,
                                                      RT_GAME3D_EFFECT_LIFETIME_MAX,
                                                      RT_GAME3D_EFFECT_LIFETIME_MAX)
                         : -1.0;
    item->age = 0.0;
    rt_obj_retain_maybe(particles);
    return particles;
}

/// @brief Register and retain a Decal3D (expires via its own lifetime); returns the
///   decal. Traps on a non-Decal3D handle.
/// @param obj EffectRegistry3D runtime handle.
/// @param decal Decal3D object to retain.
/// @return @p decal on success, or NULL after validation, capacity, or
///         allocation failure.
void *rt_game3d_effects_add_decal(void *obj, void *decal) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.addDecal: invalid effects");
    if (!effects)
        return NULL;
    if (!rt_g3d_has_class(decal, RT_G3D_DECAL3D_CLASS_ID)) {
        rt_trap("Game3D.EffectRegistry3D.addDecal: expected Decal3D");
        return NULL;
    }
    game3d_effects_repair(effects);
    if (effects->count == INT32_MAX) {
        rt_trap("Game3D.EffectRegistry3D: too many effects");
        return NULL;
    }
    if (!game3d_effects_reserve(effects, effects->count + 1))
        return NULL;
    rt_game3d_effect_item *item = &effects->items[effects->count++];
    memset(item, 0, sizeof(*item));
    item->type = RT_GAME3D_EFFECT_DECAL;
    item->object = decal;
    item->lifetime = -1.0;
    item->age = 0.0;
    rt_obj_retain_maybe(decal);
    return decal;
}

/// @brief Advance every effect by `dt`, ticking particle/decal systems and retiring
///   expired items via swap-remove (no order preserved).
/// @param obj EffectRegistry3D runtime handle.
/// @param dt Positive frame duration in seconds. Invalid, non-positive, and
///           excessive values are sanitized before any item is advanced.
void rt_game3d_effects_update(void *obj, double dt) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.update: invalid effects");
    if (!effects)
        return;
    game3d_effects_repair(effects);
    dt = game3d_positive_clamped_or(dt, 0.0, RT_GAME3D_EFFECT_STEP_MAX);
    if (dt <= 0.0)
        return;
    int32_t i = 0;
    while (i < effects->count) {
        rt_game3d_effect_item *item = &effects->items[i];
        int8_t expired = 0;
        item->age = game3d_nonnegative_clamped_or(item->age, 0.0, RT_GAME3D_EFFECT_LIFETIME_MAX);
        if (item->age > RT_GAME3D_EFFECT_LIFETIME_MAX - dt)
            item->age = RT_GAME3D_EFFECT_LIFETIME_MAX;
        else
            item->age += dt;
        if (item->type == RT_GAME3D_EFFECT_PARTICLES) {
            if (rt_g3d_has_class(item->object, RT_G3D_PARTICLES3D_CLASS_ID))
                rt_particles3d_update(item->object, dt);
            else
                expired = 1;
            if (item->lifetime >= 0.0 && item->age >= item->lifetime)
                expired = 1;
        } else if (item->type == RT_GAME3D_EFFECT_DECAL) {
            if (rt_g3d_has_class(item->object, RT_G3D_DECAL3D_CLASS_ID)) {
                rt_decal3d_update(item->object, dt);
                expired = rt_decal3d_is_expired(item->object);
            } else {
                expired = 1;
            }
        } else {
            expired = 1;
        }
        if (expired) {
            game3d_effect_release_item(item);
            if (i + 1 < effects->count)
                effects->items[i] = effects->items[effects->count - 1];
            effects->count--;
            continue;
        }
        i++;
    }
}

/// @brief Draw every effect through `canvas` (particles need `camera`; decals are
///   screen-projected by the canvas).
/// @param obj EffectRegistry3D runtime handle.
/// @param canvas Canvas3D receiving particle and decal draws; NULL is ignored.
/// @param camera Camera3D used for particle billboarding; may be NULL, in which
///               case particle entries are skipped while decals still draw.
void rt_game3d_effects_draw(void *obj, void *canvas, void *camera) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.draw: invalid effects");
    if (!effects || !canvas)
        return;
    game3d_effects_repair(effects);
    for (int32_t i = 0; i < effects->count; ++i) {
        rt_game3d_effect_item *item = &effects->items[i];
        if (item->type == RT_GAME3D_EFFECT_PARTICLES) {
            if (camera && rt_g3d_has_class(item->object, RT_G3D_PARTICLES3D_CLASS_ID))
                rt_particles3d_draw(item->object, canvas, camera);
        } else if (item->type == RT_GAME3D_EFFECT_DECAL) {
            if (rt_g3d_has_class(item->object, RT_G3D_DECAL3D_CLASS_ID))
                rt_canvas3d_draw_decal(canvas, item->object);
        }
    }
}

/// @brief Remove and release all registered effects immediately.
/// @param obj EffectRegistry3D runtime handle.
/// @post The registry contains no items but retains its allocation and PostFx3D stack.
void rt_game3d_effects_clear(void *obj) {
    rt_game3d_effects *effects =
        game3d_effects_checked(obj, "Game3D.EffectRegistry3D.clear: invalid effects");
    if (!effects)
        return;
    game3d_effects_repair(effects);
    for (int32_t i = 0; i < effects->count; ++i)
        game3d_effect_release_item(&effects->items[i]);
    effects->count = 0;
}

/// @brief Resolve and validate a world's effect registry; returns NULL if the world is
///   invalid or has none.
/// @param world_obj World3D runtime handle.
/// @param method Trap message used by world and registry validation.
/// @return The validated EffectRegistry3D owned by the world, or NULL.
static rt_game3d_effects *game3d_world_effects_checked(void *world_obj, const char *method) {
    rt_game3d_world *world = game3d_world_checked(world_obj, method);
    if (!world || !world->effects)
        return NULL;
    return game3d_effects_checked(world->effects, method);
}

/// @brief Set a particle system's emitter position from a Vec3 (NaN-scrubbed); returns
///   0 (after trapping `method`) if `position` is not a Vec3.
/// @param particles Particles3D object whose emitter position is changed.
/// @param position Vec3 position to read and sanitize.
/// @param method Trap message used when @p position is invalid.
/// @return Non-zero after applying the position, otherwise zero.
static int8_t game3d_particles_set_position_from_vec(void *particles,
                                                     void *position,
                                                     const char *method) {
    double pos[3];
    if (!game3d_read_vec3(position, pos, method))
        return 0;
    rt_particles3d_set_position(particles, pos[0], pos[1], pos[2]);
    return 1;
}

/// @brief Spawn an upward fiery explosion burst at `position`, registered with a short
///   auto-expire lifetime; returns the particle system. See header.
/// @param world_obj World3D whose effect registry owns the burst.
/// @param position Vec3 world-space emitter position.
/// @return The newly allocated and registered Particles3D, or NULL on validation
///         or allocation failure.
void *rt_game3d_effects3d_explosion(void *world_obj, void *position) {
    rt_game3d_effects *effects =
        game3d_world_effects_checked(world_obj, "Game3D.Effects3D.Explosion: invalid world");
    if (!effects)
        return NULL;
    void *particles = rt_particles3d_new(160);
    if (!particles)
        return NULL;
    if (!game3d_particles_set_position_from_vec(
            particles, position, "Game3D.Effects3D.Explosion: expected Vec3 position")) {
        game3d_release_ref(&particles);
        return NULL;
    }
    rt_particles3d_set_direction(particles, 0.0, 1.0, 0.0, 2.8);
    rt_particles3d_set_speed(particles, 3.0, 9.0);
    rt_particles3d_set_lifetime(particles, 0.25, 0.9);
    rt_particles3d_set_size(particles, 0.32, 0.04);
    rt_particles3d_set_softness(particles, 0.32);
    rt_particles3d_set_gravity(particles, 0.0, -2.0, 0.0);
    rt_particles3d_set_color(particles, 0xFFAA22, 0x442211);
    rt_particles3d_set_alpha(particles, 1.0, 0.0);
    rt_particles3d_set_additive(particles, 1);
    rt_particles3d_burst(particles, 90);
    rt_game3d_effects_add_particles(effects, particles, 1.15);
    return particles;
}

/// @brief Spawn a fast directional spark burst at `position` along `direction`; returns
///   the particle system. See header.
/// @param world_obj World3D whose effect registry owns the burst.
/// @param position Vec3 world-space emitter position.
/// @param direction Vec3 emission axis; its components are sanitized by the
///                  shared vector reader.
/// @return The newly allocated and registered Particles3D, or NULL on validation
///         or allocation failure.
void *rt_game3d_effects3d_sparks(void *world_obj, void *position, void *direction) {
    rt_game3d_effects *effects =
        game3d_world_effects_checked(world_obj, "Game3D.Effects3D.Sparks: invalid world");
    double dir[3];
    if (!effects)
        return NULL;
    if (!game3d_read_vec3(direction, dir, "Game3D.Effects3D.Sparks: expected Vec3 direction"))
        return NULL;
    void *particles = rt_particles3d_new(96);
    if (!particles)
        return NULL;
    if (!game3d_particles_set_position_from_vec(
            particles, position, "Game3D.Effects3D.Sparks: expected Vec3 position")) {
        game3d_release_ref(&particles);
        return NULL;
    }
    rt_particles3d_set_direction(particles, dir[0], dir[1], dir[2], 0.75);
    rt_particles3d_set_speed(particles, 5.0, 13.0);
    rt_particles3d_set_lifetime(particles, 0.15, 0.55);
    rt_particles3d_set_size(particles, 0.08, 0.01);
    rt_particles3d_set_softness(particles, 0.08);
    rt_particles3d_set_gravity(particles, 0.0, -7.0, 0.0);
    rt_particles3d_set_color(particles, 0xFFE88A, 0xFF6A00);
    rt_particles3d_set_alpha(particles, 1.0, 0.0);
    rt_particles3d_set_additive(particles, 1);
    rt_particles3d_burst(particles, 48);
    rt_game3d_effects_add_particles(effects, particles, 0.8);
    return particles;
}

/// @brief Spawn a slow drifting dust puff at `position`; returns the particle system. See header.
/// @param world_obj World3D whose effect registry owns the puff.
/// @param position Vec3 world-space emitter position.
/// @return The newly allocated and registered Particles3D, or NULL on validation
///         or allocation failure.
void *rt_game3d_effects3d_dust(void *world_obj, void *position) {
    rt_game3d_effects *effects =
        game3d_world_effects_checked(world_obj, "Game3D.Effects3D.Dust: invalid world");
    if (!effects)
        return NULL;
    void *particles = rt_particles3d_new(96);
    if (!particles)
        return NULL;
    if (!game3d_particles_set_position_from_vec(
            particles, position, "Game3D.Effects3D.Dust: expected Vec3 position")) {
        game3d_release_ref(&particles);
        return NULL;
    }
    rt_particles3d_set_direction(particles, 0.0, 1.0, 0.0, 1.35);
    rt_particles3d_set_speed(particles, 0.6, 2.5);
    rt_particles3d_set_lifetime(particles, 0.35, 1.2);
    rt_particles3d_set_size(particles, 0.22, 0.7);
    rt_particles3d_set_softness(particles, 0.22);
    rt_particles3d_set_gravity(particles, 0.0, 0.35, 0.0);
    rt_particles3d_set_color(particles, 0xB7AA92, 0x6D6556);
    rt_particles3d_set_alpha(particles, 0.55, 0.0);
    rt_particles3d_burst(particles, 45);
    rt_game3d_effects_add_particles(effects, particles, 1.4);
    return particles;
}

/// @brief Spawn a rising smoke plume at `position`; returns the particle system. See header.
/// @param world_obj World3D whose effect registry owns the plume.
/// @param position Vec3 world-space emitter position.
/// @return The newly allocated and registered Particles3D, or NULL on validation
///         or allocation failure.
void *rt_game3d_effects3d_smoke(void *world_obj, void *position) {
    rt_game3d_effects *effects =
        game3d_world_effects_checked(world_obj, "Game3D.Effects3D.Smoke: invalid world");
    if (!effects)
        return NULL;
    void *particles = rt_particles3d_new(128);
    if (!particles)
        return NULL;
    if (!game3d_particles_set_position_from_vec(
            particles, position, "Game3D.Effects3D.Smoke: expected Vec3 position")) {
        game3d_release_ref(&particles);
        return NULL;
    }
    rt_particles3d_set_direction(particles, 0.0, 1.0, 0.0, 1.1);
    rt_particles3d_set_speed(particles, 0.4, 1.8);
    rt_particles3d_set_lifetime(particles, 0.8, 2.0);
    rt_particles3d_set_size(particles, 0.35, 1.25);
    rt_particles3d_set_softness(particles, 0.35);
    rt_particles3d_set_gravity(particles, 0.0, 0.55, 0.0);
    rt_particles3d_set_color(particles, 0x5D6168, 0x24282E);
    rt_particles3d_set_alpha(particles, 0.45, 0.0);
    rt_particles3d_burst(particles, 55);
    rt_game3d_effects_add_particles(effects, particles, 2.2);
    return particles;
}

/// @brief Procedurally generate a 16×16 radial-falloff splat texture used as the
///   default impact-decal albedo.
/// @return A newly allocated Pixels object containing the alpha falloff, or NULL
///         when pixel storage cannot be allocated.
static void *game3d_effects_make_impact_texture(void) {
    void *pixels = rt_pixels_new(16, 16);
    if (!pixels)
        return NULL;
    for (int64_t y = 0; y < 16; ++y) {
        for (int64_t x = 0; x < 16; ++x) {
            double dx = (double)x - 7.5;
            double dy = (double)y - 7.5;
            double dist = sqrt(dx * dx + dy * dy);
            if (dist <= 7.0) {
                double edge = 1.0 - game3d_clamp(dist / 7.0, 0.0, 1.0);
                int64_t alpha = (int64_t)(edge * 210.0);
                rt_pixels_set(
                    pixels, x, y, (0x24180FFF & 0xFFFFFF00) | game3d_clamp_i64(alpha, 0, 255));
            }
        }
    }
    return pixels;
}

/// @brief Spawn a short-lived impact decal at `position` oriented to `normal` using the
///   generated splat texture; returns the decal. Traps on non-Vec3 args. See header.
/// @param world_obj World3D whose effect registry owns the decal.
/// @param position Vec3 world-space decal center.
/// @param normal Vec3 surface normal defining decal orientation.
/// @return The newly allocated and registered Decal3D, or NULL on validation or
///         allocation failure. The temporary generated texture is released
///         after Decal3D construction.
void *rt_game3d_effects3d_impact_decal(void *world_obj, void *position, void *normal) {
    rt_game3d_effects *effects =
        game3d_world_effects_checked(world_obj, "Game3D.Effects3D.ImpactDecal: invalid world");
    if (!effects)
        return NULL;
    if (!rt_g3d_is_vec3(position) || !rt_g3d_is_vec3(normal)) {
        rt_trap("Game3D.Effects3D.ImpactDecal: expected Vec3 position and normal");
        return NULL;
    }
    void *texture = game3d_effects_make_impact_texture();
    void *decal = rt_decal3d_new(position, normal, 0.55, texture);
    game3d_release_ref(&texture);
    if (!decal)
        return NULL;
    rt_decal3d_set_lifetime(decal, 2.0);
    rt_game3d_effects_add_decal(effects, decal);
    return decal;
}
