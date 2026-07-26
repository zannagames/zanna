//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_water3d.c
// Purpose: Animated water plane with sine-based waves.
//
// Key invariants:
//   - Grid resolution: configurable [8, 256], default 64x64 quads.
//   - Wave: height = amplitude * sin(freq * (x + z) - time * speed) (legacy).
//   - Gerstner multi-wave path sums up to WATER_MAX_WAVES superimposed waves.
//   - Normals computed analytically from wave derivative for smooth shading.
//   - Drawn with alpha-blended material; backface cull disabled around draw.
//
// Ownership/Lifetime:
//   - Water3D is GC-managed; finalizer releases the texture / normal-map /
//     env-map / mesh / material refs.
//
// Links: rt_water3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_water3d.c
 * @brief Implements animated grid water with legacy sine or multi-wave Gerstner deformation.
 *
 * Water3D retains a reusable mesh and material, updates analytic normals on the CPU, supports
 * texture/normal/environment bindings, and can skip off-range grid rebuilds while preserving wave
 * phase.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_water3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_heap.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_world3d_common.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_MACOS
#define WATER3D_OPTIONAL_SYMBOL __attribute__((weak_import))
#elif RT_COMPILER_GCC_LIKE
#define WATER3D_OPTIONAL_SYMBOL __attribute__((weak))
#else
#define WATER3D_OPTIONAL_SYMBOL
#endif

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *p);
extern void rt_obj_free(void *p);
/* Allocation-free camera position read (defined in rt_camera3d.c). Optional:
 * isolated contract tests link Water3D without the camera module; the
 * SetSimDistance gate simply stays inactive there. */
extern int8_t rt_camera3d_get_position_components(void *obj, double *x, double *y, double *z)
    WATER3D_OPTIONAL_SYMBOL;
#include "rt_trap.h"
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_clear(void *m);
extern void rt_mesh3d_add_vertex(
    void *m, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *m, int64_t v0, int64_t v1, int64_t v2);
extern void rt_canvas3d_draw_mesh(void *canvas, void *mesh, void *transform, void *material);
extern void rt_canvas3d_draw_mesh_matrix(void *canvas,
                                         void *mesh,
                                         const double *transform,
                                         void *material);
extern void rt_canvas3d_draw_mesh_matrix_keyed_bounds(void *canvas,
                                                      void *mesh,
                                                      const double *transform,
                                                      void *material,
                                                      const void *motion_key,
                                                      const float *prev_bone_palette,
                                                      const float *prev_morph_weights,
                                                      const float *local_bounds_min,
                                                      const float *local_bounds_max,
                                                      int8_t conservative_bounds,
                                                      int8_t disable_occlusion,
                                                      float culling_pad) WATER3D_OPTIONAL_SYMBOL;
extern void *rt_material3d_new_color(double r, double g, double b);
extern void rt_material3d_set_alpha(void *m, double a);
extern void rt_material3d_set_double_sided(void *m, int8_t enabled) WATER3D_OPTIONAL_SYMBOL;
extern void rt_material3d_set_shininess(void *m, double s);
extern void rt_material3d_set_texture(void *m, void *tex);
extern void rt_material3d_set_normal_map(void *m, void *tex);
extern void rt_material3d_set_env_map(void *m, void *cubemap);
extern void rt_material3d_set_reflectivity(void *m, double r);
extern void rt_material3d_set_ssr_enabled(void *m, int8_t enabled);
extern void rt_material3d_set_color(void *m, double r, double g, double b);

#define WATER_GRID 64
#define WATER_MAX_WAVES 8
#define WATER3D_SIZE_MAX 1000000.0
#define WATER3D_HEIGHT_ABS_MAX 1000000000000.0
#define WATER3D_PARAM_MAX 1000000.0
#define WATER3D_DT_MAX 1.0
#define WATER3D_TIME_MAX 1000000.0
#define WATER3D_TWO_PI 6.28318530717958647692

typedef struct {
    double dir[2]; /* normalized wave direction */
    double speed;
    double amplitude;
    double frequency; /* 2*PI / wavelength */
} water_wave_t;

typedef struct {
    void *vptr;
    double width, depth;
    double height;
    double center_x, center_z;
    /* Legacy single-wave params (used when wave_count == 0) */
    double wave_speed, wave_amplitude, wave_frequency;
    double color[3];
    double alpha;
    double time;
    void *mesh;
    void *material;
    /* Phase A: texture/env map support */
    void *texture;       /* surface texture (Pixels) */
    void *normal_map;    /* wave normal map (Pixels) */
    void *env_map;       /* environment cubemap for reflections */
    double reflectivity; /* [0.0-1.0] */
    /* Phase B: Gerstner multi-wave */
    water_wave_t waves[WATER_MAX_WAVES];
    int32_t wave_count;
    int32_t resolution; /* grid resolution (default WATER_GRID) */
    int8_t mesh_dirty;
    /* Distance-gated simulation (SetSimDistance): beyond this range from the
     * last drawing camera, Update advances wave TIME but skips the CPU grid
     * rebuild — re-entering cameras see correctly-phased waves without the
     * world having paid ~(res+1)^2 vertex evaluations per frame for
     * off-screen water. 0 disables the gate (always rebuild). */
    double sim_distance;
    double last_camera_pos[3];
    int8_t has_camera_pos;
} rt_water3d;

/// @brief Validate @p obj as a Water3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object handle.
/// @return Typed Water3D pointer, or NULL when the handle is invalid or has the wrong class.
static rt_water3d *water3d_checked(void *obj) {
    return (rt_water3d *)rt_g3d_checked_or_null(obj, RT_G3D_WATER3D_CLASS_ID);
}

/// @brief Return non-zero when @p pixels is a live `Zanna.Graphics.Pixels` handle.
/// @param pixels Candidate runtime object.
/// @return Nonzero when @p pixels is a valid live Pixels handle.
static int water3d_is_pixels_handle(void *pixels) {
    return rt_pixels_checked_impl_or_null(pixels) != NULL;
}

/// @brief Drop one reference and zero the slot. Idempotent on null/empty slots.
/// @param slot Address of the retained-reference slot.
static void water3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained Pixels slot only if it still points at a valid Pixels object.
/// @param slot Address of the retained Pixels slot.
static void water3d_release_pixels_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!water3d_is_pixels_handle(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    water3d_release_ref(slot);
}

/// @brief Mark the water material double-sided when the full Material3D runtime is linked.
/// @details Isolated contract tests link Water3D without the full renderer/material module. The
///   optional symbol keeps those tests on the legacy draw fallback while real runtime builds still
///   preserve the no-canvas-cull-mutation rendering path.
/// @param material Material3D handle to mark double-sided.
static void water3d_enable_double_sided_material(void *material) {
    if (material && rt_material3d_set_double_sided)
        rt_material3d_set_double_sided(material, 1);
}

/// @brief Release a retained mesh slot only if it still points at a Mesh3D object.
/// @param slot Address of the retained Mesh3D slot.
static void water3d_release_mesh_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MESH3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    water3d_release_ref(slot);
}

/// @brief Release a retained material slot only if it still points at a Material3D object.
/// @param slot Address of the retained Material3D slot.
static void water3d_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    water3d_release_ref(slot);
}

/// @brief Release a retained cubemap slot only if it still points at a Cubemap3D object.
/// @param slot Address of the retained CubeMap3D slot.
static void water3d_release_env_map_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_CUBEMAP3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    water3d_release_ref(slot);
}

/// @brief Retain-then-release assignment for Pixels slots, clearing corrupt old slots safely.
/// @param slot Address of the retained Pixels slot to update.
/// @param value New Pixels handle, or NULL to clear the slot.
static void water3d_assign_pixels_ref(void **slot, void *value) {
    if (!slot)
        return;
    if (value && !water3d_is_pixels_handle(value))
        value = NULL;
    if (*slot == value)
        return;
    if (value)
        rt_obj_retain_maybe(value);
    water3d_release_pixels_slot(slot);
    *slot = value;
}

/// @brief Retain-then-release assignment for Cubemap3D slots, clearing corrupt old slots safely.
/// @param slot Address of the retained CubeMap3D slot to update.
/// @param value New complete CubeMap3D handle, or NULL to clear the slot.
static void water3d_assign_env_map_ref(void **slot, void *value) {
    if (!slot)
        return;
    if (value &&
        (!rt_g3d_has_class(value, RT_G3D_CUBEMAP3D_CLASS_ID) || !rt_cubemap3d_is_complete(value)))
        value = NULL;
    if (*slot == value)
        return;
    if (value)
        rt_obj_retain_maybe(value);
    water3d_release_env_map_slot(slot);
    *slot = value;
}

/// @brief Clamp a double to the `[0, 1]` range — used for water knobs like transparency,
///   reflectivity, and wave amplitude that are physical [0, 1] scalars.
/// @param value Candidate normalized scalar.
/// @return Finite value clamped to `[0, 1]`.
static double water3d_clamp01(double value) {
    return rt_world3d_clamp01(value);
}

/// @brief Clamp a strictly-positive parameter to `(0, max_value]`; non-finite or ≤0 maps to
/// `fallback`.
/// @param value Candidate positive parameter.
/// @param fallback Replacement for invalid or nonpositive input.
/// @param max_value Inclusive upper bound.
/// @return Sanitized positive value.
static double water3d_clamp_positive_or(double value, double fallback, double max_value) {
    return rt_world3d_clamp_positive_or(value, fallback, max_value);
}

/// @brief Clamp `value` into `[-max_abs, max_abs]`, substituting `fallback` when not finite.
/// @param value Candidate signed parameter.
/// @param fallback Replacement for non-finite input.
/// @param max_abs Inclusive absolute-value bound.
/// @return Sanitized signed value.
static double water3d_clamp_abs_or(double value, double fallback, double max_abs) {
    return rt_world3d_clamp_abs_or(value, fallback, max_abs);
}

/// @brief Clamp `value` into `[0, max_value]`; non-finite or negative input maps to 0.
/// @param value Candidate nonnegative parameter.
/// @param max_value Inclusive upper bound.
/// @return Sanitized nonnegative value.
static double water3d_clamp_nonnegative(double value, double max_value) {
    return rt_world3d_clamp_nonnegative(value, max_value);
}

/// @brief Normalize a 2D wave direction robustly, accepting huge finite components.
/// @param x Address of the direction x component.
/// @param z Address of the direction z component.
/// @return Nonzero when a finite nonzero direction was normalized.
static int water3d_normalize_dir2(double *x, double *z) {
    if (!x || !z)
        return 0;
    *x = water3d_clamp_abs_or(*x, 0.0, WATER3D_PARAM_MAX);
    *z = water3d_clamp_abs_or(*z, 0.0, WATER3D_PARAM_MAX);
    double max_component = fmax(fabs(*x), fabs(*z));
    if (!isfinite(max_component) || max_component <= 1e-8)
        return 0;
    double sx = *x / max_component;
    double sz = *z / max_component;
    double len = sqrt(sx * sx + sz * sz);
    if (!isfinite(len) || len <= 1e-8)
        return 0;
    *x = sx / len;
    *z = sz / len;
    return 1;
}

/// @brief Release corrupted cached resources so update can recreate valid ones.
/// @param w Water surface whose retained graphics handles are repaired.
static void water3d_repair_resource_handles(rt_water3d *w) {
    if (!w)
        return;
    if (w->mesh && !rt_g3d_has_class(w->mesh, RT_G3D_MESH3D_CLASS_ID)) {
        water3d_release_mesh_slot(&w->mesh);
        w->mesh_dirty = 1;
    }
    if (w->material && !rt_g3d_has_class(w->material, RT_G3D_MATERIAL3D_CLASS_ID))
        water3d_release_material_slot(&w->material);
    if (w->texture && !water3d_is_pixels_handle(w->texture))
        water3d_release_pixels_slot(&w->texture);
    if (w->normal_map && !water3d_is_pixels_handle(w->normal_map))
        water3d_release_pixels_slot(&w->normal_map);
    if (w->env_map && (!rt_g3d_has_class(w->env_map, RT_G3D_CUBEMAP3D_CLASS_ID) ||
                       !rt_cubemap3d_is_complete(w->env_map)))
        water3d_release_env_map_slot(&w->env_map);
}

/// @brief Ensure the retained water mesh has enough storage for a direct vertex/index rewrite.
/// @details Water3D is included in isolated contract tests without the full Mesh3D implementation,
///   so this keeps the allocation path local while preserving the same internal buffer layout.
/// @param mesh Retained Mesh3D whose direct-write buffers may be grown.
/// @param vertex_capacity Required vertex capacity.
/// @param index_capacity Required index capacity.
/// @return Nonzero when both capacities are available, otherwise zero after trapping on failure.
static int water3d_mesh_reserve(rt_mesh3d *mesh,
                                uint32_t vertex_capacity,
                                uint32_t index_capacity) {
    if (!mesh)
        return 0;
    if (vertex_capacity > mesh->vertex_capacity) {
        vgfx3d_vertex_t *vertices;
        double *positions64 = mesh->positions64;
        if ((size_t)vertex_capacity > SIZE_MAX / sizeof(vgfx3d_vertex_t) ||
            (positions64 && (size_t)vertex_capacity > SIZE_MAX / (3u * sizeof(double)))) {
            rt_trap("Water3D.Update: mesh vertex allocation overflow");
            return 0;
        }
        if (positions64) {
            size_t position_values = (size_t)vertex_capacity * 3u;
            positions64 = (double *)realloc(positions64, position_values * sizeof(double));
            if (!positions64) {
                rt_trap("Water3D.Update: mesh position sidecar allocation failed");
                return 0;
            }
            mesh->positions64 = positions64;
        }
        vertices =
            (vgfx3d_vertex_t *)realloc(mesh->vertices, (size_t)vertex_capacity * sizeof(*vertices));
        if (!vertices) {
            rt_trap("Water3D.Update: mesh vertex allocation failed");
            return 0;
        }
        mesh->vertices = vertices;
        mesh->vertex_capacity = vertex_capacity;
    }
    if (index_capacity > mesh->index_capacity) {
        uint32_t *indices;
        if ((size_t)index_capacity > SIZE_MAX / sizeof(uint32_t)) {
            rt_trap("Water3D.Update: mesh index allocation overflow");
            return 0;
        }
        indices = (uint32_t *)realloc(mesh->indices, (size_t)index_capacity * sizeof(*indices));
        if (!indices) {
            rt_trap("Water3D.Update: mesh index allocation failed");
            return 0;
        }
        mesh->indices = indices;
        mesh->index_capacity = index_capacity;
    }
    return 1;
}

/// @brief Clamp one stored wave and return whether it can contribute to mesh deformation.
/// @param wv Stored wave parameters to sanitize in place.
/// @return Nonzero when the wave has a valid direction, positive amplitude, and positive frequency.
static int water3d_sanitize_wave(water_wave_t *wv) {
    if (!wv)
        return 0;
    if (!water3d_normalize_dir2(&wv->dir[0], &wv->dir[1]))
        return 0;
    wv->speed = water3d_clamp_abs_or(wv->speed, 0.0, WATER3D_PARAM_MAX);
    wv->amplitude = water3d_clamp_nonnegative(wv->amplitude, WATER3D_PARAM_MAX);
    wv->frequency = water3d_clamp_nonnegative(wv->frequency, WATER3D_PARAM_MAX);
    return wv->frequency > 0.0 && wv->amplitude > 0.0;
}

/// @brief GC finalizer: release every retained graphics resource (textures, mesh, material).
/// @param obj Water3D runtime object being finalized; NULL is accepted.
static void water3d_finalizer(void *obj) {
    rt_water3d *w = (rt_water3d *)obj;
    if (!w)
        return;
    water3d_release_pixels_slot(&w->texture);
    water3d_release_pixels_slot(&w->normal_map);
    water3d_release_env_map_slot(&w->env_map);
    water3d_release_mesh_slot(&w->mesh);
    water3d_release_material_slot(&w->material);
}

/// @brief Create a new water surface with animated wave simulation.
/// @details Creates a grid mesh that deforms each frame using sinusoidal or
///          Gerstner wave functions. The water supports configurable color,
///          transparency, surface textures, normal maps, and environment-map
///          reflections. The mesh is lazily built on first draw.
/// @param width World-space width of the water plane (X axis).
/// @param depth World-space depth of the water plane (Z axis).
/// @return Opaque water handle, or NULL on failure.
void *rt_water3d_new(double width, double depth) {
    width = water3d_clamp_positive_or(width, 1.0, WATER3D_SIZE_MAX);
    depth = water3d_clamp_positive_or(depth, 1.0, WATER3D_SIZE_MAX);
    rt_water3d *w =
        (rt_water3d *)rt_obj_new_i64(RT_G3D_WATER3D_CLASS_ID, (int64_t)sizeof(rt_water3d));
    if (!w) {
        rt_trap("Water3D.New: allocation failed");
        return NULL;
    }
    w->vptr = NULL;
    w->width = width;
    w->depth = depth;
    w->height = 0.0;
    w->center_x = 0.0;
    w->center_z = 0.0;
    w->wave_speed = 2.0;
    w->wave_amplitude = 0.15;
    w->wave_frequency = 1.5;
    w->color[0] = 0.1;
    w->color[1] = 0.3;
    w->color[2] = 0.6;
    w->alpha = 0.5;
    w->time = 0.0;
    w->mesh = NULL;
    w->material = NULL;
    w->texture = NULL;
    w->normal_map = NULL;
    w->env_map = NULL;
    w->reflectivity = 0.0;
    w->wave_count = 0;
    w->resolution = WATER_GRID;
    w->mesh_dirty = 1;
    rt_obj_set_finalizer(w, water3d_finalizer);
    return w;
}

/// @brief Set the base Y-coordinate (world height) of the water plane.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param y Finite world-space height, clamped to the supported range.
void rt_water3d_set_height(void *obj, double y) {
    rt_water3d *w = water3d_checked(obj);
    if (w) {
        w->height = water3d_clamp_abs_or(y, w->height, WATER3D_HEIGHT_ABS_MAX);
        w->mesh_dirty = 1;
    }
}

/// @brief Set the world XZ centre (and base Y height) of the water plane.
/// @details The grid is generated around the origin, so a terrain that does not
///   sit at the world origin (e.g. one spanning [0,size]) would leave the water
///   misaligned and under-resolved. Setting the centre re-centres the surface
///   over the terrain and keeps its resolution on the visible area.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param x World-space center x coordinate.
/// @param y World-space base height.
/// @param z World-space center z coordinate.
void rt_water3d_set_position(void *obj, double x, double y, double z) {
    rt_water3d *w = water3d_checked(obj);
    if (w) {
        w->center_x = water3d_clamp_abs_or(x, w->center_x, WATER3D_SIZE_MAX);
        w->height = water3d_clamp_abs_or(y, w->height, WATER3D_HEIGHT_ABS_MAX);
        w->center_z = water3d_clamp_abs_or(z, w->center_z, WATER3D_SIZE_MAX);
        w->mesh_dirty = 1;
    }
}

/// @brief Configure the sinusoidal wave animation parameters.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param speed Signed temporal phase speed.
/// @param amplitude Nonnegative vertical displacement amplitude.
/// @param frequency Nonnegative spatial angular frequency.
void rt_water3d_set_wave_params(void *obj, double speed, double amplitude, double frequency) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    w->wave_speed = water3d_clamp_abs_or(speed, 0.0, WATER3D_PARAM_MAX);
    w->wave_amplitude = water3d_clamp_nonnegative(amplitude, WATER3D_PARAM_MAX);
    w->wave_frequency = water3d_clamp_nonnegative(frequency, WATER3D_PARAM_MAX);
    w->mesh_dirty = 1;
}

/// @brief Set the base tint color and transparency of the water surface.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param r Red channel, clamped to `[0, 1]`.
/// @param g Green channel, clamped to `[0, 1]`.
/// @param b Blue channel, clamped to `[0, 1]`.
/// @param a Alpha channel, clamped to `[0, 1]`.
void rt_water3d_set_color(void *obj, double r, double g, double b, double a) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    w->color[0] = water3d_clamp01(r);
    w->color[1] = water3d_clamp01(g);
    w->color[2] = water3d_clamp01(b);
    w->alpha = water3d_clamp01(a);
}

/// @brief Set surface texture for water.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param pixels Pixels texture to retain, or NULL to clear the binding.
void rt_water3d_set_texture(void *obj, void *pixels) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    if (w->material && !rt_g3d_has_class(w->material, RT_G3D_MATERIAL3D_CLASS_ID))
        water3d_release_material_slot(&w->material);
    if (pixels && !water3d_is_pixels_handle(pixels))
        return;
    water3d_assign_pixels_ref(&w->texture, pixels);
    if (w->material)
        rt_material3d_set_texture(w->material, pixels);
}

/// @brief Set normal map for wave detail.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param pixels Pixels normal map to retain, or NULL to clear the binding.
void rt_water3d_set_normal_map(void *obj, void *pixels) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    if (w->material && !rt_g3d_has_class(w->material, RT_G3D_MATERIAL3D_CLASS_ID))
        water3d_release_material_slot(&w->material);
    if (pixels && !water3d_is_pixels_handle(pixels))
        return;
    water3d_assign_pixels_ref(&w->normal_map, pixels);
    if (w->material)
        rt_material3d_set_normal_map(w->material, pixels);
}

/// @brief Set environment cubemap for reflections.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param cubemap Complete CubeMap3D handle to retain, or NULL to clear reflections.
void rt_water3d_set_env_map(void *obj, void *cubemap) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    water3d_repair_resource_handles(w);
    if (cubemap && (!rt_g3d_has_class(cubemap, RT_G3D_CUBEMAP3D_CLASS_ID) ||
                    !rt_cubemap3d_is_complete(cubemap))) {
        if (w->material) {
            rt_material3d_set_env_map(w->material, w->env_map);
            rt_material3d_set_reflectivity(w->material, w->env_map ? w->reflectivity : 0.0);
        }
        return;
    }
    water3d_assign_env_map_ref(&w->env_map, cubemap);
    if (w->material) {
        rt_material3d_set_env_map(w->material, cubemap);
        rt_material3d_set_reflectivity(w->material, w->env_map ? w->reflectivity : 0.0);
    }
}

/// @brief Set reflectivity [0.0-1.0] for environment mapping.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param r Reflectivity clamped to `[0, 1]`; it is applied only while an environment map exists.
void rt_water3d_set_reflectivity(void *obj, double r) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    water3d_repair_resource_handles(w);
    w->reflectivity = water3d_clamp01(r);
    if (w->material)
        rt_material3d_set_reflectivity(w->material, w->env_map ? w->reflectivity : 0.0);
}

/// @brief `Water3D.SetSimDistance(distance)` — distance-gate the CPU wave rebuild.
/// @details While the camera that last drew this water is farther than
///   @p distance from the water rectangle, `Update` advances wave time but
///   skips the per-vertex grid rebuild (the dominant cost). 0 disables the
///   gate. Non-finite/negative values clamp to 0.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param distance Nonnegative camera-to-surface gate distance in world units.
void rt_water3d_set_sim_distance(void *obj, double distance) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    if (!isfinite(distance) || distance < 0.0)
        distance = 0.0;
    if (distance > WATER3D_SIZE_MAX)
        distance = WATER3D_SIZE_MAX;
    w->sim_distance = distance;
}

/// @brief `Water3D.get_SimDistance` — current simulation gate distance (0 = off).
/// @param obj Water3D handle.
/// @return Configured nonnegative gate distance, or zero for an invalid handle.
double rt_water3d_get_sim_distance(void *obj) {
    rt_water3d *w = water3d_checked(obj);
    return w ? w->sim_distance : 0.0;
}

/// @brief Set grid resolution (clamped to [8, 256]).
/// @param obj Water3D handle; invalid handles are ignored.
/// @param res Number of quads per grid axis, clamped to `[8, 256]`.
void rt_water3d_set_resolution(void *obj, int64_t res) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    if (res < 8)
        res = 8;
    if (res > 256)
        res = 256;
    if (w->resolution != (int32_t)res) {
        w->resolution = (int32_t)res;
        w->mesh_dirty = 1;
    }
}

/// @brief Add a Gerstner wave to the water.
/// @param obj Water3D handle; invalid handles are ignored.
/// @param dirX Propagation-direction x component.
/// @param dirZ Propagation-direction z component.
/// @param speed Signed temporal phase speed.
/// @param amplitude Nonnegative vertical displacement amplitude.
/// @param wavelength Positive wavelength in world units.
void rt_water3d_add_wave(
    void *obj, double dirX, double dirZ, double speed, double amplitude, double wavelength) {
    rt_water3d *w = water3d_checked(obj);
    if (!w)
        return;
    if (w->wave_count < 0)
        w->wave_count = 0;
    if (w->wave_count > WATER_MAX_WAVES)
        w->wave_count = WATER_MAX_WAVES;
    if (w->wave_count >= WATER_MAX_WAVES)
        return;
    if (!isfinite(dirX) || !isfinite(dirZ) || !isfinite(speed) || !isfinite(amplitude) ||
        !isfinite(wavelength) || amplitude < 0.0 || wavelength <= 1e-6)
        return;
    if (!water3d_normalize_dir2(&dirX, &dirZ))
        return;
    water_wave_t *wv = &w->waves[w->wave_count];
    wv->dir[0] = dirX;
    wv->dir[1] = dirZ;
    wv->speed = water3d_clamp_abs_or(speed, 0.0, WATER3D_PARAM_MAX);
    wv->amplitude = water3d_clamp_nonnegative(amplitude, WATER3D_PARAM_MAX);
    wavelength = water3d_clamp_positive_or(wavelength, 1.0, WATER3D_PARAM_MAX);
    wv->frequency = WATER3D_TWO_PI / wavelength;
    w->wave_count++;
    w->mesh_dirty = 1;
}

/// @brief Remove all Gerstner waves.
/// @param obj Water3D handle; invalid handles are ignored.
void rt_water3d_clear_waves(void *obj) {
    rt_water3d *w = water3d_checked(obj);
    if (w) {
        w->wave_count = 0;
        memset(w->waves, 0, sizeof(w->waves));
        w->mesh_dirty = 1;
    }
}

/// @brief Fill the (grid+1)^2 water vertices: position, Gerstner/legacy wave height, analytic
///        normal, and UVs. Pure transform of @p w into @p mesh; see rt_water3d_update.
/// @param w Water surface containing wave and placement parameters.
/// @param mesh Mesh3D with writable storage for the full vertex grid.
/// @param grid Number of quads per axis.
/// @param row Number of vertices per row, equal to @p grid plus one.
/// @param hx Half the water width.
/// @param hz Half the water depth.
/// @param step_x World-space x distance between adjacent vertices.
/// @param step_z World-space z distance between adjacent vertices.
/// @param inv_grid Reciprocal grid resolution used for UVs.
/// @param wave_valid Per-wave flags identifying sanitized contributing waves.
/// @param wave_time_phase Per-wave range-reduced temporal phases.
static void water3d_fill_vertices(rt_water3d *w,
                                  rt_mesh3d *mesh,
                                  int32_t grid,
                                  int32_t row,
                                  double hx,
                                  double hz,
                                  double step_x,
                                  double step_z,
                                  double inv_grid,
                                  const int8_t *wave_valid,
                                  const double *wave_time_phase) {
    for (int gz = 0; gz <= grid; gz++) {
        for (int gx = 0; gx <= grid; gx++) {
            uint32_t vertex_index = (uint32_t)(gz * row + gx);
            vgfx3d_vertex_t *vt = &mesh->vertices[vertex_index];
            double x = w->center_x - hx + gx * step_x;
            double z = w->center_z - hz + gz * step_z;
            double y = w->height;
            double dydx = 0.0, dydz = 0.0;

            if (w->wave_count > 0) {
                /* Gerstner multi-wave sum. Standard form is A·sin(k·x − ω·t):
                 * the minus sign on the time term makes crests propagate in
                 * +direction of `dir`. Previously this used `+ time*speed`,
                 * so waves visually travelled opposite the user-specified
                 * direction. */
                for (int32_t wi = 0; wi < w->wave_count; wi++) {
                    water_wave_t *wv = &w->waves[wi];
                    if (!wave_valid[wi])
                        continue;
                    double dot = wv->dir[0] * x + wv->dir[1] * z;
                    double phase = wv->frequency * dot - wave_time_phase[wi];
                    if (!isfinite(phase))
                        continue;
                    phase = fmod(phase, WATER3D_TWO_PI);
                    if (!isfinite(phase))
                        phase = 0.0;
                    y += wv->amplitude * sin(phase);
                    double dc = wv->amplitude * wv->frequency * cos(phase);
                    dydx += dc * wv->dir[0];
                    dydz += dc * wv->dir[1];
                }
            } else {
                /* Legacy single sine wave — keep same sign convention as above. */
                double phase = w->wave_frequency * (x + z) - w->time * w->wave_speed;
                if (!isfinite(phase))
                    phase = 0.0;
                phase = fmod(phase, WATER3D_TWO_PI);
                if (!isfinite(phase))
                    phase = 0.0;
                y += w->wave_amplitude * sin(phase);
                double dc = w->wave_amplitude * w->wave_frequency * cos(phase);
                dydx = dc;
                dydz = dc;
            }

            if (!isfinite(y))
                y = w->height;
            if (!isfinite(dydx))
                dydx = 0.0;
            if (!isfinite(dydz))
                dydz = 0.0;

            /* Normal from wave derivatives */
            double nx = -dydx, ny = 1.0, nz = -dydz;
            double nlen = sqrt(nx * nx + ny * ny + nz * nz);
            if (isfinite(nlen) && nlen > 1e-8) {
                nx /= nlen;
                ny /= nlen;
                nz /= nlen;
            } else {
                nx = 0.0;
                ny = 1.0;
                nz = 0.0;
            }

            double u = (double)gx * inv_grid;
            double v = (double)gz * inv_grid;
            memset(vt, 0, sizeof(*vt));
            vt->pos[0] = (float)x;
            vt->pos[1] = (float)y;
            vt->pos[2] = (float)z;
            vt->normal[0] = (float)nx;
            vt->normal[1] = (float)ny;
            vt->normal[2] = (float)nz;
            vt->uv[0] = (float)u;
            vt->uv[1] = (float)v;
            vt->uv1[0] = (float)u;
            vt->uv1[1] = (float)v;
            vt->color[0] = 1.0f;
            vt->color[1] = 1.0f;
            vt->color[2] = 1.0f;
            vt->color[3] = 1.0f;
            vt->tangent[3] = 1.0f;
            if (mesh->positions64) {
                mesh->positions64[(size_t)vertex_index * 3u + 0] = x;
                mesh->positions64[(size_t)vertex_index * 3u + 1] = y;
                mesh->positions64[(size_t)vertex_index * 3u + 2] = z;
            }
        }
    }
}

/// @brief Emit the 2*grid*grid triangle indices for the water grid (two triangles per cell).
/// @param mesh Mesh3D with writable storage for all grid indices.
/// @param grid Number of quads per axis.
/// @param row Number of vertices per row.
static void water3d_fill_indices(rt_mesh3d *mesh, int32_t grid, int32_t row) {
    uint32_t out_index = 0;
    for (int gz = 0; gz < grid; gz++) {
        for (int gx = 0; gx < grid; gx++) {
            uint32_t base = (uint32_t)(gz * row + gx);
            mesh->indices[out_index++] = base;
            mesh->indices[out_index++] = base + (uint32_t)row;
            mesh->indices[out_index++] = base + 1u;
            mesh->indices[out_index++] = base + 1u;
            mesh->indices[out_index++] = base + (uint32_t)row;
            mesh->indices[out_index++] = base + (uint32_t)row + 1u;
        }
    }
}

/// @brief Create the water material on first use or refresh its color/alpha/texture/env bindings.
/// @param w Water surface owning material state and retained texture bindings.
/// @return 0 when the material could not be allocated (caller aborts without clearing mesh_dirty).
static int water3d_update_material(rt_water3d *w) {
    if (!w->material) {
        w->material = rt_material3d_new_color(w->color[0], w->color[1], w->color[2]);
        if (!w->material)
            return 0;
        rt_material3d_set_shininess(w->material, 128.0);
    } else {
        rt_material3d_set_color(w->material, w->color[0], w->color[1], w->color[2]);
    }
    rt_material3d_set_alpha(w->material, w->alpha);
    water3d_enable_double_sided_material(w->material);
    rt_material3d_set_texture(w->material, w->texture);
    rt_material3d_set_normal_map(w->material, w->normal_map);
    rt_material3d_set_env_map(w->material, w->env_map);
    rt_material3d_set_reflectivity(w->material, w->env_map ? w->reflectivity : 0.0);
    /* Plan 10: water opts into screen-space reflections; backends without SSR
     * (or chains without an SSR pass) simply keep the env-map term above. */
    rt_material3d_set_ssr_enabled(w->material, 1);
    return 1;
}

/// @brief Advance the water simulation by `dt` seconds and regenerate the surface mesh.
/// @details Rebuilds the (grid+1)x(grid+1) vertex grid and the 2*grid*grid triangle list
///          each frame. Vertex heights come from either the Gerstner multi-wave sum (when
///          waves have been registered via `rt_water3d_add_wave`) or a single legacy sine
///          wave. Per-vertex normals are derived analytically from the wave derivative
///          (dy/dx, dy/dz) rather than numerically, keeping shading smooth across the
///          grid independent of resolution.
///
///          Mesh and material are created lazily on the first update and then reused:
///          `rt_mesh3d_clear` resets contents in place to avoid per-frame GC churn, and
///          the material's color/alpha/texture/env-map bindings are pushed through every
///          update so live property edits take effect on the next frame. Reflectivity is
///          forced to 0 when no environment cubemap is bound so the shader skips the
///          reflection path.
///
///          No work is done when `dt <= 0` — this lets callers pause the simulation by
///          passing dt=0 without redundant mesh rebuilds.
/// @param obj Opaque water handle from `rt_water3d_new` (no-op when NULL).
/// @param dt Elapsed seconds since the previous update (must be > 0 to apply).
void rt_water3d_update(void *obj, double dt) {
    rt_water3d *w = water3d_checked(obj);
    if (!w || !isfinite(dt) || dt < 0.0)
        return;
    water3d_repair_resource_handles(w);
    if (dt > WATER3D_DT_MAX)
        dt = WATER3D_DT_MAX;
    if (dt == 0.0 && !w->mesh_dirty && w->mesh && w->material)
        return;
    w->time += dt;
    if (!isfinite(w->time))
        w->time = 0.0;
    if (w->time > WATER3D_TIME_MAX)
        w->time = fmod(w->time, WATER3D_TIME_MAX);

    /* Distance gate: when the last drawing camera is beyond sim_distance of
     * the water's surface rectangle, keep the phase advance above but skip the
     * grid rebuild entirely. First build (no mesh yet) always runs so the
     * water has geometry to draw when it comes into range. */
    if (w->sim_distance > 0.0 && w->has_camera_pos && w->mesh && w->material && !w->mesh_dirty) {
        double half_w = w->width * 0.5;
        double half_d = w->depth * 0.5;
        double dx = fmax(fabs(w->last_camera_pos[0] - w->center_x) - half_w, 0.0);
        double dy = w->last_camera_pos[1] - w->height;
        double dz = fmax(fabs(w->last_camera_pos[2] - w->center_z) - half_d, 0.0);
        double dist_sq = dx * dx + dy * dy + dz * dz;
        if (isfinite(dist_sq) && dist_sq > w->sim_distance * w->sim_distance)
            return;
    }

    w->width = water3d_clamp_positive_or(w->width, 1.0, WATER3D_SIZE_MAX);
    w->depth = water3d_clamp_positive_or(w->depth, 1.0, WATER3D_SIZE_MAX);
    w->height = water3d_clamp_abs_or(w->height, 0.0, WATER3D_HEIGHT_ABS_MAX);
    w->wave_speed = water3d_clamp_abs_or(w->wave_speed, 0.0, WATER3D_PARAM_MAX);
    w->wave_amplitude = water3d_clamp_nonnegative(w->wave_amplitude, WATER3D_PARAM_MAX);
    w->wave_frequency = water3d_clamp_nonnegative(w->wave_frequency, WATER3D_PARAM_MAX);
    w->reflectivity = water3d_clamp01(w->reflectivity);
    w->alpha = water3d_clamp01(w->alpha);
    for (int32_t c = 0; c < 3; c++)
        w->color[c] = water3d_clamp01(w->color[c]);
    if (w->resolution < 8 || w->resolution > 256)
        w->resolution = WATER_GRID;
    if (w->wave_count < 0)
        w->wave_count = 0;
    if (w->wave_count > WATER_MAX_WAVES)
        w->wave_count = WATER_MAX_WAVES;
    /* Sanitize each wave once up front (idempotent) and cache its validity plus
     * a range-reduced time phase. Doing this here — rather than per vertex —
     * removes O(grid^2 * wave_count) redundant work each rebuild, and the
     * fmod keeps per-vertex phase precision intact after long runtimes. */
    int8_t wave_valid[WATER_MAX_WAVES];
    double wave_time_phase[WATER_MAX_WAVES];
    for (int32_t wi = 0; wi < w->wave_count; wi++) {
        wave_valid[wi] = water3d_sanitize_wave(&w->waves[wi]) ? 1 : 0;
        double tp = fmod(w->time * w->waves[wi].speed, WATER3D_TWO_PI);
        wave_time_phase[wi] = isfinite(tp) ? tp : 0.0;
    }

    /* Regenerate mesh with new wave positions (reuse allocation to avoid GC pressure). */
    if (!w->mesh)
        w->mesh = rt_mesh3d_new();
    if (!w->mesh)
        return;
    int32_t grid = w->resolution;
    int32_t row = grid + 1;
    uint32_t required_vertices = (uint32_t)(row * row);
    uint32_t required_indices = (uint32_t)(grid * grid * 6);
    rt_mesh3d *mesh = (rt_mesh3d *)w->mesh;
    int topology_dirty =
        w->mesh_dirty || !mesh->vertices || !mesh->indices ||
        mesh->vertex_capacity < required_vertices || mesh->index_capacity < required_indices ||
        mesh->vertex_count != required_vertices || mesh->index_count != required_indices;
    double inv_grid = 1.0 / (double)grid; /* resolution clamped to [8, 256] above */
    double hx = w->width * 0.5, hz = w->depth * 0.5;
    double step_x = w->width * inv_grid;
    double step_z = w->depth * inv_grid;
    if (topology_dirty) {
        rt_mesh3d_clear(w->mesh);
        if (!water3d_mesh_reserve(mesh, required_vertices, required_indices)) {
            w->mesh_dirty = 1;
            return;
        }
        if (!mesh->vertices || !mesh->indices || mesh->vertex_capacity < required_vertices ||
            mesh->index_capacity < required_indices) {
            w->mesh_dirty = 1;
            return;
        }
    }
    mesh->vertex_count = required_vertices;
    mesh->index_count = required_indices;
    mesh->build_failed = 0;

    /* Vertices */
    water3d_fill_vertices(
        w, mesh, grid, row, hx, hz, step_x, step_z, inv_grid, wave_valid, wave_time_phase);

    if (topology_dirty)
        water3d_fill_indices(mesh, grid, row);
    rt_mesh3d_touch_geometry(mesh);
    if (((rt_mesh3d *)w->mesh)->build_failed) {
        rt_mesh3d_clear(w->mesh);
        w->mesh_dirty = 1;
        return;
    }

    /* Update material — create on first use, update properties every frame */
    if (!water3d_update_material(w))
        return;
    w->mesh_dirty = 0;
}

/// @brief Draw the animated water surface through the 3D canvas.
/// @details Emits the current water mesh at the world origin (the grid is already built
///          in world space around the origin by `rt_water3d_update`, so an identity model
///          matrix is correct — reusing a static table avoids allocating a Transform3D).
///          The material is marked double-sided so the underside of waves and any
///          submerged camera angle still show geometry without mutating canvas-wide culling.
///          Does nothing if
///          the mesh/material haven't been built yet (i.e. `update` was never called) or
///          if `canvas` / `obj` are NULL. When available, @p camera supplies the last observed
///          position used by distance-gated simulation updates.
/// @param canvas Canvas3D handle receiving the water draw.
/// @param obj Water3D handle supplying current mesh and material state.
/// @param camera Optional Camera3D handle used to update the simulation-distance reference point.
void rt_canvas3d_draw_water(void *canvas, void *obj, void *camera) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    rt_water3d *w = water3d_checked(obj);
    if (!c || !w)
        return;
    /* Remember the drawing camera's position for the SetSimDistance gate in
     * rt_water3d_update (allocation-free component read; optional symbol —
     * see the extern above). */
    {
        double cx;
        double cy;
        double cz;
        if (camera && rt_camera3d_get_position_components &&
            rt_camera3d_get_position_components(camera, &cx, &cy, &cz)) {
            w->last_camera_pos[0] = cx;
            w->last_camera_pos[1] = cy;
            w->last_camera_pos[2] = cz;
            w->has_camera_pos = 1;
        }
    }
    water3d_repair_resource_handles(w);
    if (!w->mesh || !w->material || w->mesh_dirty)
        rt_water3d_update(w, 0.0);
    if (!w->mesh || !w->material)
        return;

    static const double identity[16] = {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    rt_mesh3d *mesh = (rt_mesh3d *)w->mesh;
    water3d_enable_double_sided_material(w->material);
    if (rt_canvas3d_draw_mesh_matrix_keyed_bounds) {
        rt_mesh3d_refresh_bounds(mesh);
        rt_canvas3d_draw_mesh_matrix_keyed_bounds(c,
                                                  w->mesh,
                                                  identity,
                                                  w->material,
                                                  w,
                                                  NULL,
                                                  NULL,
                                                  mesh->aabb_min,
                                                  mesh->aabb_max,
                                                  0,
                                                  1,
                                                  0.0f);
    } else {
        rt_canvas3d_draw_mesh_matrix(c, w->mesh, identity, w->material);
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
