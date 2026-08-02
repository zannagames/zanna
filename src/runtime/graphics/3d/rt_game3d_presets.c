//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/rt_game3d_presets.c
// Purpose: One-call scene presets for the Zanna.Game3D layer — lighting rigs,
//   material presets, post-FX looks, quality tiers, and primitive prefab spawns.
//   Split out of rt_game3d.c; shares private types/helpers via rt_game3d_internal.h.
// Links: rt_game3d_internal.h, rt_light3d.h, rt_material3d.h, rt_postfx3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements one-call Game3D lighting, material, post-processing, quality, and prefab
/// presets.
/// @details Presets sanitize public inputs, build the underlying renderer objects,
/// transfer or release temporary references explicitly, and install coherent groups
/// of settings without exposing the private World3D payload.

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
#include "rt_pixels_internal.h"
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

/// @brief Remove all preset lights and reset to a dim neutral ambient. See header.
/// @param obj Borrowed live World3D handle whose Canvas3D lighting is reset.
void rt_game3d_lighting_clear(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.Lighting.Clear: invalid world");
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    if (!canvas)
        return;
    rt_canvas3d_clear_lights(canvas);
    rt_canvas3d_set_ambient(canvas, 0.18, 0.18, 0.20);
}

/// @brief Install a neutral two-light (key + fill) studio rig with a dark backdrop. See header.
/// @param obj Borrowed live World3D handle receiving the rig.
void rt_game3d_lighting_studio(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.Lighting.Studio: invalid world");
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    if (!canvas)
        return;
    void *key_dir = rt_vec3_new(-0.35, -0.85, -0.30);
    void *fill_dir = rt_vec3_new(0.75, -0.35, 0.40);
    void *key = key_dir ? rt_light3d_new_directional(key_dir, 1.0, 0.96, 0.88) : NULL;
    void *fill = fill_dir ? rt_light3d_new_directional(fill_dir, 0.55, 0.65, 1.0) : NULL;
    if (!key_dir || !fill_dir || !key || !fill)
        goto cleanup;
    if (key)
        rt_light3d_set_intensity(key, 1.35);
    if (fill)
        rt_light3d_set_intensity(fill, 0.35);
    rt_game3d_lighting_clear(world);
    rt_canvas3d_set_ambient(canvas, 0.30, 0.32, 0.36);
    game3d_world_set_clear_color(world, 0.055, 0.060, 0.070);
    game3d_world_install_light(world, 0, key);
    game3d_world_install_light(world, 1, fill);
cleanup:
    game3d_release_ref(&key);
    game3d_release_ref(&fill);
    game3d_release_ref(&key_dir);
    game3d_release_ref(&fill_dir);
}

/// @brief Install a single bright sun light and sky-blue backdrop; a NULL `sun_dir`
///   uses a default down-angled direction. Traps on a non-Vec3 direction. See header.
/// @param obj Borrowed live World3D handle receiving the rig.
/// @param sun_dir Borrowed Vec3 direction to normalize, or `NULL` for the preset direction.
void rt_game3d_lighting_outdoor(void *obj, void *sun_dir) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.Lighting.Outdoor: invalid world");
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    if (!canvas)
        return;
    double dir_xyz[3] = {-0.45, -1.0, -0.22};
    if (sun_dir) {
        if (!game3d_read_vec3(sun_dir, dir_xyz, "Game3D.Lighting.Outdoor: sunDir must be Vec3"))
            return;
    }
    double dir_len = hypot(hypot(dir_xyz[0], dir_xyz[1]), dir_xyz[2]);
    if (!isfinite(dir_len) || dir_len <= 1e-12) {
        dir_xyz[0] = -0.45;
        dir_xyz[1] = -1.0;
        dir_xyz[2] = -0.22;
        dir_len = hypot(hypot(dir_xyz[0], dir_xyz[1]), dir_xyz[2]);
    }
    if (isfinite(dir_len) && dir_len > 1e-12) {
        dir_xyz[0] /= dir_len;
        dir_xyz[1] /= dir_len;
        dir_xyz[2] /= dir_len;
    }

    void *dir = rt_vec3_new(dir_xyz[0], dir_xyz[1], dir_xyz[2]);
    void *sun = dir ? rt_light3d_new_directional(dir, 1.0, 0.94, 0.82) : NULL;
    if (!dir || !sun)
        goto cleanup;
    if (sun)
        rt_light3d_set_intensity(sun, 1.55);
    rt_game3d_lighting_clear(world);
    rt_canvas3d_set_ambient(canvas, 0.38, 0.42, 0.46);
    game3d_world_set_clear_color(world, 0.50, 0.66, 0.86);
    game3d_world_install_light(world, 0, sun);
cleanup:
    game3d_release_ref(&sun);
    game3d_release_ref(&dir);
}

/// @brief Install a dim moonlight + cool point lamp for a dark night look. See header.
/// @param obj Borrowed live World3D handle receiving the rig.
void rt_game3d_lighting_night(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.Lighting.Night: invalid world");
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    if (!canvas)
        return;
    void *moon_dir = rt_vec3_new(0.25, -1.0, 0.35);
    void *lamp_pos = rt_vec3_new(0.0, 4.0, 2.0);
    void *moon = moon_dir ? rt_light3d_new_directional(moon_dir, 0.55, 0.68, 1.0) : NULL;
    void *lamp = lamp_pos ? rt_light3d_new_point(lamp_pos, 0.55, 0.64, 1.0, 0.12) : NULL;
    if (!moon_dir || !lamp_pos || !moon || !lamp)
        goto cleanup;
    if (moon)
        rt_light3d_set_intensity(moon, 0.55);
    if (lamp)
        rt_light3d_set_intensity(lamp, 0.80);
    rt_game3d_lighting_clear(world);
    rt_canvas3d_set_ambient(canvas, 0.045, 0.055, 0.095);
    game3d_world_set_clear_color(world, 0.015, 0.020, 0.040);
    game3d_world_install_light(world, 0, moon);
    game3d_world_install_light(world, 1, lamp);
cleanup:
    game3d_release_ref(&moon);
    game3d_release_ref(&lamp);
    game3d_release_ref(&moon_dir);
    game3d_release_ref(&lamp_pos);
}

/// @brief Install a warm key + cool rim point-light pair for indoor scenes. See header.
/// @param obj Borrowed live World3D handle receiving the rig.
void rt_game3d_lighting_interior(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.Lighting.Interior: invalid world");
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    if (!canvas)
        return;
    void *key_pos = rt_vec3_new(0.0, 4.0, 2.5);
    void *rim_pos = rt_vec3_new(-3.5, 2.0, -2.0);
    void *key = key_pos ? rt_light3d_new_point(key_pos, 1.0, 0.78, 0.52, 0.08) : NULL;
    void *rim = rim_pos ? rt_light3d_new_point(rim_pos, 0.50, 0.62, 1.0, 0.12) : NULL;
    if (!key_pos || !rim_pos || !key || !rim)
        goto cleanup;
    if (key)
        rt_light3d_set_intensity(key, 1.25);
    if (rim)
        rt_light3d_set_intensity(rim, 0.45);
    rt_game3d_lighting_clear(world);
    rt_canvas3d_set_ambient(canvas, 0.22, 0.20, 0.18);
    game3d_world_set_clear_color(world, 0.055, 0.052, 0.048);
    game3d_world_install_light(world, 0, key);
    game3d_world_install_light(world, 1, rim);
cleanup:
    game3d_release_ref(&key);
    game3d_release_ref(&rim);
    game3d_release_ref(&key_pos);
    game3d_release_ref(&rim_pos);
}

/// @brief Build an opaque PBR material from a clamped color, metallic, and roughness,
///   shared by the material presets below.
/// @param r Red base-color channel, clamped to `[0, 1]`.
/// @param g Green base-color channel, clamped to `[0, 1]`.
/// @param b Blue base-color channel, clamped to `[0, 1]`.
/// @param metallic Metallic factor, clamped to `[0, 1]`.
/// @param roughness Roughness factor, clamped to `[0, 1]`.
/// @return New GC-managed opaque Material3D handle, or `NULL` on allocation failure.
static void *game3d_material_pbr(double r, double g, double b, double metallic, double roughness) {
    void *mat = rt_material3d_new_pbr(
        game3d_clamp(r, 0.0, 1.0), game3d_clamp(g, 0.0, 1.0), game3d_clamp(b, 0.0, 1.0));
    if (mat) {
        rt_material3d_set_shading_model(mat, RT_GAME3D_SHADING_PBR);
        rt_material3d_set_metallic(mat, game3d_clamp(metallic, 0.0, 1.0));
        rt_material3d_set_roughness(mat, game3d_clamp(roughness, 0.0, 1.0));
        rt_material3d_set_ao(mat, 1.0);
        rt_material3d_set_alpha(mat, 1.0);
        rt_material3d_set_alpha_mode(mat, RT_GAME3D_ALPHA_OPAQUE);
    }
    return mat;
}

/// @brief Matte dielectric plastic preset (non-metallic, medium roughness). See header.
/// @param r Red base-color channel, clamped to `[0, 1]`.
/// @param g Green base-color channel, clamped to `[0, 1]`.
/// @param b Blue base-color channel, clamped to `[0, 1]`.
/// @return New GC-managed Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_plastic(double r, double g, double b) {
    return game3d_material_pbr(r, g, b, 0.0, 0.46);
}

/// @brief Shiny metallic preset (full metallic, low roughness, some reflectivity). See header.
/// @param r Red base-color channel, clamped to `[0, 1]`.
/// @param g Green base-color channel, clamped to `[0, 1]`.
/// @param b Blue base-color channel, clamped to `[0, 1]`.
/// @return New GC-managed Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_metal(double r, double g, double b) {
    void *mat = game3d_material_pbr(r, g, b, 1.0, 0.22);
    if (mat)
        rt_material3d_set_reflectivity(mat, 0.35);
    return mat;
}

/// @brief Soft matte rubber preset (non-metallic, high roughness). See header.
/// @param r Red base-color channel, clamped to `[0, 1]`.
/// @param g Green base-color channel, clamped to `[0, 1]`.
/// @param b Blue base-color channel, clamped to `[0, 1]`.
/// @return New GC-managed Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_rubber(double r, double g, double b) {
    return game3d_material_pbr(r, g, b, 0.0, 0.88);
}

/// @brief Translucent double-sided glass preset (blended, reflective). See header.
/// @param r Red base-color channel, clamped to `[0, 1]`.
/// @param g Green base-color channel, clamped to `[0, 1]`.
/// @param b Blue base-color channel, clamped to `[0, 1]`.
/// @param alpha Opacity clamped to `[0.05, 1]`.
/// @return New GC-managed blended Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_glass(double r, double g, double b, double alpha) {
    void *mat = game3d_material_pbr(r, g, b, 0.0, 0.08);
    if (mat) {
        rt_material3d_set_alpha(mat, game3d_clamp(alpha, 0.05, 1.0));
        rt_material3d_set_alpha_mode(mat, RT_GAME3D_ALPHA_BLEND);
        rt_material3d_set_double_sided(mat, 1);
        rt_material3d_set_reflectivity(mat, 0.50);
    }
    return mat;
}

/// @brief Self-illuminated emissive preset at the given color/intensity. See header.
/// @param r Red emissive and base-color channel, clamped to `[0, 1]`.
/// @param g Green emissive and base-color channel, clamped to `[0, 1]`.
/// @param b Blue emissive and base-color channel, clamped to `[0, 1]`.
/// @param intensity Non-negative emissive multiplier; invalid input falls back to one.
/// @return New GC-managed emissive Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_emissive(double r, double g, double b, double intensity) {
    r = game3d_clamp(r, 0.0, 1.0);
    g = game3d_clamp(g, 0.0, 1.0);
    b = game3d_clamp(b, 0.0, 1.0);
    void *mat = rt_material3d_new_color(r, g, b);
    if (mat) {
        rt_material3d_set_shading_model(mat, RT_GAME3D_SHADING_EMISSIVE);
        rt_material3d_set_emissive_color(mat, r, g, b);
        rt_material3d_set_emissive_intensity(
            mat, game3d_nonnegative_clamped_or(intensity, 1.0, RT_GAME3D_SCALE_ABS_MAX));
    }
    return mat;
}

/// @brief Flat unlit preset that ignores scene lighting. See header.
/// @param r Red base-color channel, clamped to `[0, 1]`.
/// @param g Green base-color channel, clamped to `[0, 1]`.
/// @param b Blue base-color channel, clamped to `[0, 1]`.
/// @return New GC-managed unlit Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_unlit(double r, double g, double b) {
    void *mat = rt_material3d_new_color(
        game3d_clamp(r, 0.0, 1.0), game3d_clamp(g, 0.0, 1.0), game3d_clamp(b, 0.0, 1.0));
    if (mat) {
        rt_material3d_set_unlit(mat, 1);
        rt_material3d_set_shading_model(mat, RT_GAME3D_SHADING_UNLIT);
    }
    return mat;
}

/// @brief PBR material sampling its albedo from a Pixels texture. See header.
/// @param pixels Borrowed Pixels texture passed to the material constructor.
/// @return New GC-managed textured Material3D handle, or `NULL` on allocation failure.
void *rt_game3d_materials_from_albedo_map(void *pixels) {
    if (!rt_pixels_checked_impl_or_null(pixels)) {
        rt_trap("Game3D.Materials.FromAlbedoMap: pixels must be Pixels");
        return NULL;
    }
    void *mat = rt_material3d_new_textured(pixels);
    if (mat) {
        rt_material3d_set_shading_model(mat, RT_GAME3D_SHADING_PBR);
        rt_material3d_set_metallic(mat, 0.0);
        rt_material3d_set_roughness(mat, 0.55);
        rt_material3d_set_ao(mat, 1.0);
    }
    return mat;
}

/// @brief Install a cinematic post-FX chain (bloom, tone-map, FXAA, color-grade,
///   vignette) on the world. See header.
/// @param obj Borrowed live World3D handle receiving the new stack.
void rt_game3d_postfx_cinematic(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.PostFX.Cinematic: invalid world");
    if (!world)
        return;
    void *fx = rt_postfx3d_new();
    if (!fx)
        return;
    rt_postfx3d_add_bloom(fx, 0.78, 0.22, 2);
    rt_postfx3d_add_tonemap(fx, 2, 1.10);
    rt_postfx3d_add_fxaa(fx);
    rt_postfx3d_add_color_grade(fx, 0.015, 1.08, 1.06);
    rt_postfx3d_add_vignette(fx, 0.96, 0.28);
    game3d_world_assign_postfx(world, fx);
    game3d_release_ref(&fx);
}

/// @brief Install a light, minimal post-FX chain (subtle tone-map, FXAA, color-grade)
///   for a crisp look. See header.
/// @param obj Borrowed live World3D handle receiving the new stack.
void rt_game3d_postfx_crisp(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.PostFX.Crisp: invalid world");
    if (!world)
        return;
    void *fx = rt_postfx3d_new();
    if (!fx)
        return;
    rt_postfx3d_add_tonemap(fx, 1, 1.02);
    rt_postfx3d_add_fxaa(fx);
    rt_postfx3d_add_color_grade(fx, 0.0, 1.05, 1.02);
    game3d_world_assign_postfx(world, fx);
    game3d_release_ref(&fx);
}

/// @brief Disable all post-processing by installing a disabled post-FX stack. See header.
/// @param obj Borrowed live World3D handle receiving the disabled stack.
void rt_game3d_postfx_none(void *obj) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.PostFX.None: invalid world");
    if (!world)
        return;
    void *fx = rt_postfx3d_new();
    if (!fx)
        return;
    rt_postfx3d_set_enabled(fx, 0);
    game3d_world_assign_postfx(world, fx);
    game3d_release_ref(&fx);
}

/// @brief Apply a quality preset: out-of-range values default to BALANCED; enables
///   frustum culling, and configures or disables shadows (resolution/bias scaled by
///   preset) based on backend support. See header.
/// @param obj Borrowed live World3D handle whose renderer settings are updated.
/// @param quality Requested `RT_GAME3D_QUALITY_*` value.
void rt_game3d_quality_apply(void *obj, int64_t quality) {
    rt_game3d_world *world = game3d_world_checked(obj, "Game3D.Quality.Apply: invalid world");
    void *canvas = world ? rt_g3d_checked_or_null(world->canvas, RT_G3D_CANVAS3D_CLASS_ID) : NULL;
    if (!canvas)
        return;
    if (quality < RT_GAME3D_QUALITY_PERFORMANCE || quality > RT_GAME3D_QUALITY_CINEMATIC)
        quality = RT_GAME3D_QUALITY_BALANCED;

    rt_game3d_world_set_quality(world, quality);
    rt_canvas3d_set_frustum_culling(canvas, 1);
    if (quality == RT_GAME3D_QUALITY_PERFORMANCE) {
        rt_canvas3d_disable_shadows(canvas);
        return;
    }

    rt_string shadows_capability = rt_const_cstr("shadows");
    int8_t shadows_supported =
        shadows_capability ? rt_canvas3d_backend_supports(canvas, shadows_capability) : 0;
    rt_string_unref(shadows_capability);
    if (shadows_supported) {
        rt_canvas3d_enable_shadows(canvas, quality == RT_GAME3D_QUALITY_CINEMATIC ? 2048 : 1024);
        rt_canvas3d_set_shadow_bias(canvas, quality == RT_GAME3D_QUALITY_CINEMATIC ? 0.003 : 0.005);
        rt_canvas3d_set_shadow_slope_bias(canvas,
                                          quality == RT_GAME3D_QUALITY_CINEMATIC ? 0.75 : 1.0);
        rt_string csm_capability = rt_const_cstr("shadow-csm");
        int8_t csm_supported =
            csm_capability ? rt_canvas3d_backend_supports(canvas, csm_capability) : 0;
        rt_string_unref(csm_capability);
        if (csm_supported)
            rt_canvas3d_set_shadow_cascades(canvas, quality == RT_GAME3D_QUALITY_CINEMATIC ? 4 : 2);
        else
            rt_canvas3d_set_shadow_cascades(canvas, 1);
    } else {
        rt_canvas3d_disable_shadows(canvas);
    }
}

/// @brief Clamp a requested tessellation segment count to [8, 256], using `fallback`
///   (itself floored at 8) when the request is too low.
/// @param segments Requested segment count.
/// @param fallback Replacement used below the minimum.
/// @return Segment count in `[8, 256]`.
static int64_t game3d_sanitize_segments(int64_t segments, int64_t fallback) {
    if (fallback < 8)
        fallback = 8;
    else if (fallback > 256)
        fallback = 256;
    if (segments < 8)
        return fallback;
    if (segments > 256)
        return 256;
    return segments;
}

/// @brief Validate a borrowed optional prefab material before mesh allocation.
/// @param material Optional Material3D handle.
/// @return Non-zero for NULL or Material3D; zero after recording a trap otherwise.
static int game3d_prefab_material_is_valid(void *material) {
    if (!material ||
        rt_obj_is_instance(material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d)))
        return 1;
    rt_trap("Game3D.Prefab: material must be Material3D");
    return 0;
}

/// @brief Wrap a freshly built mesh into a named entity, supplying a default plastic
///   material when none is given; consumes the mesh reference (and the default material).
/// @param mesh Owned Mesh3D reference consumed by this helper.
/// @param material Borrowed Material3D handle, or `NULL` to allocate a default plastic material.
/// @param name Null-terminated entity name, or `NULL` to leave the default name.
/// @return New GC-managed Entity3D handle, or `NULL` on construction failure.
static void *game3d_prefab_from_mesh(void *mesh, void *material, const char *name) {
    int owns_material = 0;
    if (!rt_obj_is_instance(mesh, RT_G3D_MESH3D_CLASS_ID, sizeof(rt_mesh3d))) {
        if (mesh)
            rt_trap("Game3D.Prefab: generated mesh is invalid");
        return NULL;
    }
    if (material &&
        !rt_obj_is_instance(material, RT_G3D_MATERIAL3D_CLASS_ID, sizeof(rt_material3d))) {
        game3d_release_typed_ref(&mesh, RT_G3D_MESH3D_CLASS_ID);
        rt_trap("Game3D.Prefab: material must be Material3D");
        return NULL;
    }
    if (!material) {
        material = rt_game3d_materials_plastic(0.72, 0.74, 0.76);
        owns_material = 1;
        if (!material) {
            game3d_release_typed_ref(&mesh, RT_G3D_MESH3D_CLASS_ID);
            return NULL;
        }
    }
    void *entity = rt_game3d_entity_of(mesh, material);
    if (entity && name) {
        rt_string runtime_name = rt_const_cstr(name);
        rt_game3d_entity_set_name(entity, runtime_name);
        rt_string_unref(runtime_name);
    }
    game3d_release_typed_ref(&mesh, RT_G3D_MESH3D_CLASS_ID);
    if (owns_material)
        game3d_release_ref(&material);
    return entity;
}

/// @brief Create a uniform cube entity of the given size. See header.
/// @param size Positive edge length; invalid input falls back to one.
/// @param material Borrowed Material3D handle, or `NULL` for default plastic.
/// @return New GC-managed Entity3D handle, or `NULL` on allocation failure.
void *rt_game3d_prefab_box(double size, void *material) {
    if (!game3d_prefab_material_is_valid(material))
        return NULL;
    double s = game3d_positive_clamped_or(size, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    return game3d_prefab_from_mesh(rt_mesh3d_new_box(s, s, s), material, "Box");
}

/// @brief Create a box entity with explicit width/height/depth. See header.
/// @param width Positive X extent; invalid input falls back to one.
/// @param height Positive Y extent; invalid input falls back to one.
/// @param depth Positive Z extent; invalid input falls back to one.
/// @param material Borrowed Material3D handle, or `NULL` for default plastic.
/// @return New GC-managed Entity3D handle, or `NULL` on allocation failure.
void *rt_game3d_prefab_box_xyz(double width, double height, double depth, void *material) {
    if (!game3d_prefab_material_is_valid(material))
        return NULL;
    double w = game3d_positive_clamped_or(width, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    double h = game3d_positive_clamped_or(height, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    double d = game3d_positive_clamped_or(depth, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    return game3d_prefab_from_mesh(rt_mesh3d_new_box(w, h, d), material, "BoxXYZ");
}

/// @brief Create a UV-sphere entity (segments clamped, default 32). See header.
/// @param radius Positive sphere radius; invalid input falls back to `0.5`.
/// @param segments Requested tessellation count, sanitized to the supported range.
/// @param material Borrowed Material3D handle, or `NULL` for default plastic.
/// @return New GC-managed Entity3D handle, or `NULL` on allocation failure.
void *rt_game3d_prefab_sphere(double radius, int64_t segments, void *material) {
    if (!game3d_prefab_material_is_valid(material))
        return NULL;
    double r = game3d_positive_clamped_or(radius, 0.5, RT_GAME3D_SCALE_ABS_MAX);
    return game3d_prefab_from_mesh(
        rt_mesh3d_new_sphere(r, game3d_sanitize_segments(segments, 32)), material, "Sphere");
}

/// @brief Create a cylinder entity (segments clamped, default 24). See header.
/// @param radius Positive cylinder radius; invalid input falls back to `0.5`.
/// @param height Positive cylinder height; invalid input falls back to one.
/// @param segments Requested tessellation count, sanitized to the supported range.
/// @param material Borrowed Material3D handle, or `NULL` for default plastic.
/// @return New GC-managed Entity3D handle, or `NULL` on allocation failure.
void *rt_game3d_prefab_cylinder(double radius, double height, int64_t segments, void *material) {
    if (!game3d_prefab_material_is_valid(material))
        return NULL;
    double r = game3d_positive_clamped_or(radius, 0.5, RT_GAME3D_SCALE_ABS_MAX);
    double h = game3d_positive_clamped_or(height, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    return game3d_prefab_from_mesh(
        rt_mesh3d_new_cylinder(r, h, game3d_sanitize_segments(segments, 24)), material, "Cylinder");
}

/// @brief Create a flat plane entity of the given footprint. See header.
/// @param width Positive X extent; invalid input falls back to one.
/// @param depth Positive Z extent; invalid input falls back to one.
/// @param material Borrowed Material3D handle, or `NULL` for default plastic.
/// @return New GC-managed Entity3D handle, or `NULL` on allocation failure.
void *rt_game3d_prefab_plane(double width, double depth, void *material) {
    if (!game3d_prefab_material_is_valid(material))
        return NULL;
    double w = game3d_positive_clamped_or(width, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    double d = game3d_positive_clamped_or(depth, 1.0, RT_GAME3D_SCALE_ABS_MAX);
    return game3d_prefab_from_mesh(rt_mesh3d_new_plane(w, d), material, "Plane");
}

/// @brief Create a large ground plane named "Ground" on the WORLD layer. See header.
/// @param size Positive square footprint size; invalid input falls back to one per axis.
/// @param material Borrowed Material3D handle, or `NULL` for default plastic.
/// @return New GC-managed Entity3D handle on the world layer, or `NULL` on allocation failure.
void *rt_game3d_prefab_ground(double size, void *material) {
    void *entity = rt_game3d_prefab_plane(size, size, material);
    if (entity) {
        rt_string name = rt_const_cstr("Ground");
        rt_game3d_entity_set_name(entity, name);
        rt_string_unref(name);
        rt_game3d_entity_set_layer(entity, RT_GAME3D_LAYER_WORLD);
    }
    return entity;
}
