//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_vegetation3d.c
// Purpose: Instanced vegetation rendering with cross-billboard blades,
//   wind animation, density map population, and distance-based LOD.
//
// Key invariants:
//   - Blade mesh: 2 perpendicular quads (8 verts, 4 tris) — cross-billboard.
//   - Population: LCG random scatter on terrain, filtered by density map.
//   - Wind: per-blade Y-axis shear using sin(position + time).
//   - LOD: monotone per-blade thinning with a scale fade-out approaching the
//     far distance; culling walks a coarse XZ grid, not the full population.
//   - Rendering: queues instanced blade draws through Canvas3D so they share
//     the same shadow, sorting, and overlay pipeline as other 3D content.
//   - Backface culling temporarily disabled for grass (visible both sides).
//
// Ownership/Lifetime:
//   - Vegetation3D is GC-managed; finalizer frees per-instance buffers and
//     releases the blade mesh, blade material, and density map.
//
// Links: rt_vegetation3d.h, rt_terrain3d.h, rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_vegetation3d.c
 * @brief Implements deterministic terrain scattering, wind animation, LOD culling, and drawing.
 *
 * Vegetation instances share one cross-billboard mesh and material. A persistent CSR grid narrows
 * per-frame work to cells intersecting the camera's far radius, after which stable hash thinning
 * and scale fading build a compact instanced draw batch.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_vegetation3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_terrain3d_internal.h"
#include "rt_world3d_common.h"
#include "vgfx3d_backend.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VEGETATION3D_BLADE_DIM_MAX 1000000.0
#define VEGETATION3D_TERRAIN_EXTENT_MAX 1000000.0
#define VEGETATION3D_WIND_PARAM_MAX 1000000.0
#define VEGETATION3D_TIME_MAX 1000000.0
#define VEGETATION3D_DT_MAX 1.0
#define VEGETATION3D_MAX_BLADES 2000000
#define VEGETATION3D_TWO_PI 6.28318530717958647692
#define VEGETATION3D_LOD_FADE_START 0.85f /* band position where the scale fade-out begins */
#define VEGETATION3D_LOD_FADE_FLOOR 0.02f /* below this scale a blade is invisible: drop it */
#define VEGETATION3D_GRID_MAX_CELLS 128   /* per-axis cap for the spatial cull grid */

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern void *rt_mesh3d_new(void);
extern void rt_mesh3d_add_vertex(
    void *m, double x, double y, double z, double nx, double ny, double nz, double u, double v);
extern void rt_mesh3d_add_triangle(void *m, int64_t v0, int64_t v1, int64_t v2);
extern void rt_mesh3d_clear(void *m);
extern void *rt_mat4_identity(void);
extern void rt_canvas3d_draw_mesh(void *canvas, void *mesh, void *transform, void *material);
extern void rt_canvas3d_queue_instanced_batch_bounds(void *canvas_obj,
                                                     void *mesh_obj,
                                                     void *material_obj,
                                                     const float *instance_matrices,
                                                     int32_t instance_count,
                                                     const float *prev_instance_matrices,
                                                     int8_t has_prev_instance_matrices,
                                                     const float *local_bounds_min,
                                                     const float *local_bounds_max,
                                                     int8_t conservative_bounds,
                                                     int8_t disable_occlusion);
extern void *rt_material3d_new(void);
extern void rt_material3d_set_texture(void *m, void *tex);
extern void rt_material3d_set_unlit(void *m, int8_t u);
extern double rt_terrain3d_get_height_at(void *terrain, double x, double z);

typedef struct {
    void *vptr;
    void *blade_mesh;     /* cross-billboard mesh (shared by all blades) */
    void *blade_material; /* textured, unlit */
    /* Blade geometry params */
    double blade_width;
    double blade_height;
    double size_variation; /* [0.0-1.0] random scale variation */
    /* Population */
    float *base_transforms; /* total_count * 16 floats */
    float *positions;       /* total_count * 3 floats (x,y,z for wind/LOD) */
    int32_t total_count;
    int32_t capacity;
    void *density_map;
    /* Wind */
    double wind_speed;
    double wind_strength;
    double wind_turbulence;
    double time;
    /* LOD */
    float lod_near;
    float lod_far;
    /* Per-frame visible set */
    float *visible_transforms; /* visible_count * 16 floats */
    int32_t visible_count;
    int32_t visible_capacity;
    uint32_t scatter_seed;
    /* Spatial cull grid (rebuilt lazily whenever the population changes) */
    int32_t *grid_cell_start; /* cells_x*cells_z + 1 CSR offsets into grid_indices */
    int32_t *grid_indices;    /* blade indices bucketed by XZ cell */
    int32_t grid_cells_x;
    int32_t grid_cells_z;
    float grid_min_x;
    float grid_min_z;
    float grid_cell_w;
    float grid_cell_d;
    int32_t grid_ready; /* 0 → rebuild before the next culling pass */
} rt_vegetation3d;

/// @brief Return @p value when finite, else @p fallback. Sanitizes scalar inputs.
/// @param value Candidate scalar.
/// @param fallback Replacement for non-finite input.
/// @return @p value when finite, otherwise @p fallback.
static double vegetation_finite_or(double value, double fallback) {
    return rt_world3d_finite_or(value, fallback);
}

/// @brief Clamp a positive vegetation scalar to a bounded finite range.
/// @param value Candidate positive scalar.
/// @param fallback Replacement for invalid or nonpositive input.
/// @param max_value Inclusive upper limit.
/// @return Sanitized positive scalar.
static double vegetation_positive_or(double value, double fallback, double max_value) {
    return rt_world3d_clamp_positive_or(value, fallback, max_value);
}

/// @brief Clamp a non-negative vegetation scalar to a bounded finite range.
/// @param value Candidate nonnegative scalar.
/// @param max_value Inclusive upper limit.
/// @return Sanitized scalar in `[0, max_value]`.
static double vegetation_nonnegative_or(double value, double max_value) {
    return rt_world3d_clamp_nonnegative(value, max_value);
}

/// @brief Clamp a signed vegetation scalar to a bounded finite range.
/// @param value Candidate signed scalar.
/// @param fallback Replacement for non-finite input.
/// @param max_abs Inclusive absolute-value limit.
/// @return Sanitized scalar in `[-max_abs, max_abs]`.
static double vegetation_abs_or(double value, double fallback, double max_abs) {
    return rt_world3d_clamp_abs_or(value, fallback, max_abs);
}

/// @brief Drop one GC reference held in `*slot` and clear the slot. NULL-safe.
/// @param slot Address of the retained-reference slot.
static void vegetation3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Return true when @p pixels is a live Pixels object.
/// @param pixels Candidate runtime object.
/// @return Nonzero when @p pixels is a valid live Pixels handle.
static int vegetation3d_is_pixels_handle(void *pixels) {
    return rt_pixels_checked_impl_or_null(pixels) != NULL;
}

/// @brief Release a retained Pixels slot only if it still points at Pixels.
/// @param slot Address of the retained Pixels slot.
static void vegetation3d_release_pixels_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!vegetation3d_is_pixels_handle(*slot)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    vegetation3d_release_ref(slot);
}

/// @brief Release a retained Mesh3D slot only if it still points at Mesh3D.
/// @param slot Address of the retained Mesh3D slot.
static void vegetation3d_release_mesh_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MESH3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    vegetation3d_release_ref(slot);
}

/// @brief Release a retained Material3D slot only if it still points at Material3D.
/// @param slot Address of the retained Material3D slot.
static void vegetation3d_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    vegetation3d_release_ref(slot);
}

/// @brief Retain-then-release swap into a Pixels slot. Safe on self-assign.
/// @param slot Address of the retained Pixels slot to update.
/// @param value New Pixels handle, or NULL to clear the slot.
static void vegetation3d_assign_pixels_ref(void **slot, void *value) {
    if (!slot)
        return;
    if (value && !vegetation3d_is_pixels_handle(value))
        value = NULL;
    if (*slot == value)
        return;
    if (value)
        rt_obj_retain_maybe(value);
    vegetation3d_release_pixels_slot(slot);
    *slot = value;
}

/// @brief Return a Mesh3D slot only when its private handle is still valid.
/// @param ref Candidate private mesh handle.
/// @return Borrowed typed Mesh3D pointer, or NULL when invalid.
static rt_mesh3d *vegetation3d_mesh_ref(void *ref) {
    return rt_g3d_has_class(ref, RT_G3D_MESH3D_CLASS_ID) ? (rt_mesh3d *)ref : NULL;
}

/// @brief Return a Material3D slot only when its private handle is still valid.
/// @param ref Candidate private material handle.
/// @return Borrowed typed Material3D pointer, or NULL when invalid.
static rt_material3d *vegetation3d_material_ref(void *ref) {
    return rt_g3d_has_class(ref, RT_G3D_MATERIAL3D_CLASS_ID) ? (rt_material3d *)ref : NULL;
}

/// @brief Mark a blade material double-sided so crossed grass planes render from either face.
/// @param mat Material3D to update; NULL is accepted.
static void vegetation3d_enable_double_sided_material(rt_material3d *mat) {
    if (!mat)
        return;
    mat->double_sided = 1;
}

/// @brief Integer avalanche hash used for stable vegetation LOD thinning.
/// @details The input combines the blade index and quantized position so thinning stays stable
///   across frames but does not keep every Nth blade in a visible grid/banding pattern.
/// @param x Input bits to avalanche.
/// @return Deterministically mixed 32-bit hash.
static uint32_t vegetation3d_hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// @brief Deterministic per-blade random in [0, 1) used for monotone distance thinning.
/// @details Combines the blade index with its quantized position so the value is stable across
///   frames and free of the banding a plain index-modulo pattern would produce. Comparing this
///   fixed value against a density threshold that falls with distance makes the kept set strictly
///   shrink as the camera recedes — each blade winks out at exactly one radius instead of whole
///   cohorts swapping in and out at stride boundaries.
/// @param index Stable blade index.
/// @param bx Quantized blade x coordinate input.
/// @param bz Quantized blade z coordinate input.
/// @return Stable pseudorandom value in the half-open interval `[0, 1)`.
static float vegetation3d_lod_hash01(int32_t index, float bx, float bz) {
    uint32_t qx = (uint32_t)((int32_t)lrintf(bx * 16.0f));
    uint32_t qz = (uint32_t)((int32_t)lrintf(bz * 16.0f));
    uint32_t h = vegetation3d_hash_u32((uint32_t)index ^ vegetation3d_hash_u32(qx) ^
                                       (vegetation3d_hash_u32(qz) << 1));
    return (float)h * (1.0f / 4294967296.0f);
}

/// @brief Smooth, layered wind wave for one blade.
/// @details A single full-strength sine makes thin billboard grass read like it has two hard
///   endpoints. Blending slower spatially-offset waves keeps the public strength value intact
///   while making nearby blades drift through the sway instead of moving in lockstep.
/// @param phase Base temporal and spatial phase.
/// @param bx Blade x coordinate.
/// @param bz Blade z coordinate.
/// @return Finite layered wave amplitude.
static double vegetation3d_wind_wave(double phase, double bx, double bz) {
    double primary = sin(phase);
    double broad = sin(phase * 0.37 + bx * 0.021 - bz * 0.017);
    double ripple = sin(phase * 1.19 + bx * 0.009 + bz * 0.013);
    double wave = primary * 0.62 + broad * 0.28 + ripple * 0.10;
    return isfinite(wave) ? wave : 0.0;
}

/// @brief Release and clear corrupted retained resource slots.
/// @param v Vegetation system whose density, mesh, and material handles are repaired.
static void vegetation3d_repair_resource_handles(rt_vegetation3d *v) {
    if (!v)
        return;
    if (v->density_map && !vegetation3d_is_pixels_handle(v->density_map))
        vegetation3d_release_pixels_slot(&v->density_map);
    if (v->blade_mesh && !vegetation3d_mesh_ref(v->blade_mesh))
        vegetation3d_release_mesh_slot(&v->blade_mesh);
    if (v->blade_material && !vegetation3d_material_ref(v->blade_material))
        vegetation3d_release_material_slot(&v->blade_material);
}

/// @brief Free the spatial cull grid and mark it for rebuild.
/// @param v Vegetation system owning the CSR grid arrays.
static void vegetation3d_free_grid(rt_vegetation3d *v) {
    if (!v)
        return;
    free(v->grid_cell_start);
    free(v->grid_indices);
    v->grid_cell_start = NULL;
    v->grid_indices = NULL;
    v->grid_cells_x = 0;
    v->grid_cells_z = 0;
    v->grid_ready = 0;
}

/// @brief Free all population buffers and reset the population/visible counters.
/// @param v Vegetation system whose blade and visible-set allocations are cleared.
static void vegetation3d_clear_population_buffers(rt_vegetation3d *v) {
    if (!v)
        return;
    free(v->base_transforms);
    free(v->positions);
    free(v->visible_transforms);
    v->base_transforms = NULL;
    v->positions = NULL;
    v->visible_transforms = NULL;
    v->total_count = 0;
    v->capacity = 0;
    v->visible_count = 0;
    v->visible_capacity = 0;
    vegetation3d_free_grid(v);
}

/// @brief GC finalizer — release owned blade resources and instance arrays.
/// @details The vegetation system stores per-instance data in three
///   independent flat buffers: `base_transforms` (original 4x4 matrices),
///   `positions` (vec3 source points kept for wind resimulation), and
///   `visible_transforms` (this frame's culled subset). All three are
///   plain float allocations — nothing inside them owns downstream refs,
///   so the finalize is just three `free`s plus pointer nulling.
/// @param obj Vegetation3D runtime object being finalized; NULL is accepted.
static void vegetation3d_finalizer(void *obj) {
    rt_vegetation3d *v = (rt_vegetation3d *)obj;
    if (!v)
        return;
    vegetation3d_clear_population_buffers(v);
    vegetation3d_release_pixels_slot(&v->density_map);
    vegetation3d_release_mesh_slot(&v->blade_mesh);
    vegetation3d_release_material_slot(&v->blade_material);
}

/// @brief Build the cross-billboard blade mesh (2 perpendicular quads).
/// @param mesh Mesh3D receiving eight vertices and four triangles.
/// @param w Positive blade width.
/// @param h Positive blade height.
static void build_blade_mesh(void *mesh, double w, double h) {
    if (!mesh)
        return;
    rt_mesh3d_begin_geometry_batch((rt_mesh3d *)mesh);
    w = vegetation_positive_or(w, 0.4, VEGETATION3D_BLADE_DIM_MAX);
    h = vegetation_positive_or(h, 1.2, VEGETATION3D_BLADE_DIM_MAX);
    double hw = w * 0.5;
    /* Quad 1: X-aligned */
    rt_mesh3d_add_vertex(mesh, -hw, 0, 0, 0, 0, 1, 0, 1);
    rt_mesh3d_add_vertex(mesh, hw, 0, 0, 0, 0, 1, 1, 1);
    rt_mesh3d_add_vertex(mesh, hw, h, 0, 0, 0, 1, 1, 0);
    rt_mesh3d_add_vertex(mesh, -hw, h, 0, 0, 0, 1, 0, 0);
    rt_mesh3d_add_triangle(mesh, 0, 1, 2);
    rt_mesh3d_add_triangle(mesh, 0, 2, 3);
    /* Quad 2: Z-aligned (perpendicular) */
    rt_mesh3d_add_vertex(mesh, 0, 0, -hw, 1, 0, 0, 0, 1);
    rt_mesh3d_add_vertex(mesh, 0, 0, hw, 1, 0, 0, 1, 1);
    rt_mesh3d_add_vertex(mesh, 0, h, hw, 1, 0, 0, 1, 0);
    rt_mesh3d_add_vertex(mesh, 0, h, -hw, 1, 0, 0, 0, 0);
    rt_mesh3d_add_triangle(mesh, 4, 5, 6);
    rt_mesh3d_add_triangle(mesh, 4, 6, 7);
    rt_mesh3d_end_geometry_batch((rt_mesh3d *)mesh);
}

/// @brief Deterministic linear congruential RNG for scattering blade instances.
/// @details Uses the `glibc` constants (1103515245, 12345, mod 2^32). Chosen
///   over `rand()` so blade placement is repeatable across platforms and
///   runs for a given seed — important for authoring workflows where an
///   artist tunes a seed to get a specific look. Not suitable for anything
///   cryptographic, but statistically adequate for even-ish scattering.
/// @param state Address of the mutable non-cryptographic RNG state.
/// @return Next 32-bit pseudorandom value, or zero when @p state is NULL.
static uint32_t lcg_next(uint32_t *state) {
    if (!state)
        return 0u;
    *state = *state * 1103515245u + 12345u;
    return *state;
}

/// @brief Mix a user/default seed into the non-zero 32-bit LCG state space.
/// @param seed Signed user or internal seed bits.
/// @return Deterministically mixed nonzero 32-bit LCG state.
static uint32_t vegetation3d_seed_from_i64(int64_t seed) {
    uint64_t x = (uint64_t)seed;
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53);
    x ^= x >> 33;
    uint32_t out = (uint32_t)x ^ (uint32_t)(x >> 32);
    return out ? out : 0x6D2B79F5u;
}

/// @brief Generate a non-zero default scatter seed for a new vegetation object.
/// @details Uses a process-local monotonic counter so separate Vegetation3D instances no longer
///   share identical layouts unless the author explicitly calls `SetSeed`.
/// @return Nonzero default scatter seed.
static uint32_t vegetation3d_next_default_seed(void) {
    static int64_t counter = INT64_C(0x51ED270B);
    int64_t old = rt_atomic_fetch_add_i64(&counter, INT64_C(0x1E3779B97F4A7C1), __ATOMIC_RELAXED);
    return vegetation3d_seed_from_i64(old);
}

/// @brief Repair count/buffer invariants before update/draw work touches flat arrays.
/// @param v Vegetation system whose flat-buffer counts and handles are repaired.
/// @return 1 after repair, or 0 when @p v is NULL.
static int vegetation3d_repair_state(rt_vegetation3d *v) {
    if (!v)
        return 0;
    vegetation3d_repair_resource_handles(v);
    if (v->capacity < 0)
        v->capacity = 0;
    if (!v->base_transforms || !v->positions || v->capacity == 0) {
        free(v->base_transforms);
        free(v->positions);
        v->base_transforms = NULL;
        v->positions = NULL;
        v->total_count = 0;
        v->capacity = 0;
    } else {
        if (v->total_count < 0)
            v->total_count = 0;
        if (v->total_count > v->capacity)
            v->total_count = v->capacity;
    }
    if (v->visible_capacity < 0)
        v->visible_capacity = 0;
    if (!v->visible_transforms || v->visible_capacity == 0) {
        free(v->visible_transforms);
        v->visible_transforms = NULL;
        v->visible_count = 0;
        v->visible_capacity = 0;
    } else {
        if (v->visible_count < 0)
            v->visible_count = 0;
        if (v->visible_count > v->visible_capacity)
            v->visible_count = v->visible_capacity;
    }
    return 1;
}

/// @brief True if a transform matrix is finite and inside the vegetation world range.
/// @param m Sixteen-float row-major transform to validate.
/// @return Nonzero when all matrix components are finite and bounded.
static int vegetation3d_matrix_is_drawable(const float *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]) || fabsf(m[i]) > (float)VEGETATION3D_TERRAIN_EXTENT_MAX)
            return 0;
    }
    return 1;
}

/// @brief Remove invalid visible matrices before handing the batch to Canvas3D.
/// @param v Vegetation system whose visible transform prefix is compacted in place.
static void vegetation3d_compact_visible(rt_vegetation3d *v) {
    if (!v || !v->visible_transforms || v->visible_count <= 0) {
        if (v)
            v->visible_count = 0;
        return;
    }
    int32_t out = 0;
    for (int32_t i = 0; i < v->visible_count; i++) {
        float *src = &v->visible_transforms[(size_t)i * 16u];
        if (!vegetation3d_matrix_is_drawable(src))
            continue;
        if (out != i)
            memcpy(&v->visible_transforms[(size_t)out * 16u], src, 16u * sizeof(float));
        out++;
    }
    v->visible_count = out;
}

/// @brief Construct a Vegetation3D system. Allocates the shared cross-billboard blade mesh and
/// an unlit textured material (if `blade_texture` is non-NULL — opacity comes from the texture).
/// Defaults: 0.4×1.2 blades with 30% size variation, wind speed 2.0 / strength 0.15 / turbulence
/// 0.5, LOD near=40 / far=100 world units. Traps on allocation failure.
/// @param blade_texture Optional Pixels texture retained by the generated unlit material.
/// @return New GC-managed Vegetation3D handle, or NULL after validation or allocation failure.
void *rt_vegetation3d_new(void *blade_texture) {
    if (blade_texture && !vegetation3d_is_pixels_handle(blade_texture)) {
        rt_trap("Vegetation3D.New: blade_texture must be Pixels");
        return NULL;
    }
    rt_vegetation3d *v = (rt_vegetation3d *)rt_obj_new_i64(RT_G3D_VEGETATION3D_CLASS_ID,
                                                           (int64_t)sizeof(rt_vegetation3d));
    if (!v) {
        rt_trap("Vegetation3D.New: allocation failed");
        return NULL;
    }
    v->vptr = NULL;
    v->blade_width = 0.4;
    v->blade_height = 1.2;
    v->size_variation = 0.3;
    v->base_transforms = NULL;
    v->positions = NULL;
    v->total_count = 0;
    v->capacity = 0;
    v->density_map = NULL;
    v->wind_speed = 2.0;
    v->wind_strength = 0.15;
    v->wind_turbulence = 0.5;
    v->time = 0.0;
    v->lod_near = 40.0f;
    v->lod_far = 100.0f;
    v->visible_transforms = NULL;
    v->visible_count = 0;
    v->visible_capacity = 0;
    v->scatter_seed = vegetation3d_next_default_seed();
    v->grid_cell_start = NULL;
    v->grid_indices = NULL;
    v->grid_cells_x = 0;
    v->grid_cells_z = 0;
    v->grid_min_x = 0.0f;
    v->grid_min_z = 0.0f;
    v->grid_cell_w = 1.0f;
    v->grid_cell_d = 1.0f;
    v->grid_ready = 0;

    /* Build blade mesh */
    v->blade_mesh = rt_mesh3d_new();
    if (!v->blade_mesh) {
        if (rt_obj_release_check0(v))
            rt_obj_free(v);
        rt_trap("Vegetation3D.New: blade mesh allocation failed");
        return NULL;
    }
    build_blade_mesh(v->blade_mesh, v->blade_width, v->blade_height);

    /* Build material */
    v->blade_material = rt_material3d_new();
    if (!v->blade_material) {
        vegetation3d_release_mesh_slot(&v->blade_mesh);
        if (rt_obj_release_check0(v))
            rt_obj_free(v);
        rt_trap("Vegetation3D.New: material allocation failed");
        return NULL;
    }
    if (blade_texture)
        rt_material3d_set_texture(v->blade_material, blade_texture);
    rt_material3d_set_unlit(v->blade_material, 1);

    rt_obj_set_finalizer(v, vegetation3d_finalizer);
    return v;
}

/// @brief Attach a Pixels density map. During `_populate`, each candidate blade rolls against the
/// red channel of the corresponding pixel — higher R = denser vegetation. NULL = uniform density.
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param pixels Nonempty Pixels density map to retain, or NULL for uniform density.
void rt_vegetation3d_set_density_map(void *obj, void *pixels) {
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v)
        return;
    if (pixels) {
        rt_pixels_impl *p =
            rt_pixels_checked_impl(pixels, "Vegetation3D.SetDensityMap: expected Pixels");
        if (!p || p->width <= 0 || p->height <= 0 || !p->data) {
            rt_trap("Vegetation3D.SetDensityMap: density map must be non-empty Pixels");
            return;
        }
    }
    vegetation3d_assign_pixels_ref(&v->density_map, pixels);
}

/// @brief Configure wind animation. `speed` scales time, `strength` is the maximum top-of-blade
/// shear in world units, `turbulence` controls how much the wind phase varies across the field
/// (higher = more chaotic, less synchronized waving).
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param speed Signed temporal wave speed, bounded to the supported range.
/// @param strength Nonnegative maximum blade shear in world units.
/// @param turbulence Nonnegative spatial phase variation.
void rt_vegetation3d_set_wind_params(void *obj, double speed, double strength, double turbulence) {
    if (!obj)
        return;
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v)
        return;
    v->wind_speed = vegetation_abs_or(speed, 0.0, VEGETATION3D_WIND_PARAM_MAX);
    v->wind_strength = vegetation_nonnegative_or(strength, VEGETATION3D_WIND_PARAM_MAX);
    v->wind_turbulence = vegetation_nonnegative_or(turbulence, VEGETATION3D_WIND_PARAM_MAX);
}

/// @brief Set LOD thresholds. Within `near_dist` all blades render; between near and far they
/// progressively thin (skip every 1..5 instances based on distance ratio); beyond `far_dist`
/// blades are hard-culled. Larger near = denser foreground at higher GPU cost.
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param near_dist Full-density radius in world units.
/// @param far_dist Hard-cull radius, sanitized to remain greater than @p near_dist.
void rt_vegetation3d_set_lod_distances(void *obj, double near_dist, double far_dist) {
    if (!obj)
        return;
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v)
        return;
    near_dist = vegetation_nonnegative_or(near_dist, VEGETATION3D_TERRAIN_EXTENT_MAX);
    far_dist = vegetation_nonnegative_or(far_dist, VEGETATION3D_TERRAIN_EXTENT_MAX);
    if (near_dist <= 0.0)
        near_dist = 40.0;
    if (far_dist <= 0.0)
        far_dist = 100.0;
    if (far_dist <= near_dist + 1e-6)
        far_dist = near_dist + 1.0;
    if (far_dist > VEGETATION3D_TERRAIN_EXTENT_MAX)
        far_dist = VEGETATION3D_TERRAIN_EXTENT_MAX;
    if (near_dist >= far_dist)
        near_dist = far_dist > 1.0 ? far_dist - 1.0 : 0.0;
    v->lod_near = (float)near_dist;
    v->lod_far = (float)far_dist;
}

/// @brief Reset blade dimensions and rebuild the shared mesh. `variation` ∈ [0,1] randomizes per-
/// blade scale at populate time. Already-populated blades retain their previous random scale —
/// call `_populate` again to apply the new variation factor.
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param width Positive cross-billboard width.
/// @param height Positive cross-billboard height.
/// @param variation Per-instance scale variation clamped to `[0, 1]`.
void rt_vegetation3d_set_blade_size(void *obj, double width, double height, double variation) {
    if (!obj)
        return;
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v)
        return;
    width = vegetation_positive_or(width, 0.4, VEGETATION3D_BLADE_DIM_MAX);
    height = vegetation_positive_or(height, 1.2, VEGETATION3D_BLADE_DIM_MAX);
    variation = vegetation_finite_or(variation, 0.0);
    if (variation < 0.0)
        variation = 0.0;
    if (variation > 1.0)
        variation = 1.0;
    v->blade_width = width;
    v->blade_height = height;
    v->size_variation = variation;
    /* Rebuild blade mesh with new size */
    if (v->blade_mesh && !vegetation3d_mesh_ref(v->blade_mesh))
        vegetation3d_release_mesh_slot(&v->blade_mesh);
    if (!v->blade_mesh) {
        v->blade_mesh = rt_mesh3d_new();
        if (!v->blade_mesh) {
            rt_trap("Vegetation3D.SetBladeSize: blade mesh allocation failed");
            return;
        }
    } else {
        rt_mesh3d_clear(v->blade_mesh);
    }
    build_blade_mesh(v->blade_mesh, width, height);
}

/// @brief Set the deterministic scatter seed used by subsequent Populate calls.
/// @details The seed is stored on the Vegetation3D object; calling Populate repeatedly after the
///   same seed produces the same candidate sequence, while different objects no longer default to
///   the same hardcoded layout.
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param seed User seed mixed into the nonzero 32-bit scatter state.
void rt_vegetation3d_set_seed(void *obj, int64_t seed) {
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v)
        return;
    v->scatter_seed = vegetation3d_seed_from_i64(seed);
}

/// @brief Build a row-major 4x4 transform: translate(x,y,z) * rotateY(angle) * scale(s).
/// @param out Sixteen-float output matrix.
/// @param x World-space x translation.
/// @param y World-space y translation.
/// @param z World-space z translation.
/// @param angle Y-axis rotation in radians.
/// @param s Positive uniform scale.
static void build_transform(float *out, double x, double y, double z, double angle, double s) {
    if (!out)
        return;
    x = vegetation_abs_or(x, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    y = vegetation_abs_or(y, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    z = vegetation_abs_or(z, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    angle = vegetation_abs_or(angle, 0.0, VEGETATION3D_WIND_PARAM_MAX);
    s = vegetation_positive_or(s, 1.0, VEGETATION3D_BLADE_DIM_MAX);
    angle = fmod(angle, VEGETATION3D_TWO_PI);
    if (!isfinite(angle))
        angle = 0.0;
    double ca = cos(angle), sa = sin(angle);
    if (!isfinite(ca))
        ca = 1.0;
    if (!isfinite(sa))
        sa = 0.0;
    /* Row 0 */ out[0] = (float)(ca * s);
    out[1] = 0;
    out[2] = (float)(sa * s);
    out[3] = (float)x;
    /* Row 1 */ out[4] = 0;
    out[5] = (float)s;
    out[6] = 0;
    out[7] = (float)y;
    /* Row 2 */ out[8] = (float)(-sa * s);
    out[9] = 0;
    out[10] = (float)(ca * s);
    out[11] = (float)z;
    /* Row 3 */ out[12] = 0;
    out[13] = 0;
    out[14] = 0;
    out[15] = 1.0f;
}

/// @brief Scatter up to `count` blade instances across `terrain`. Positions are LCG-randomized
/// within terrain bounds (2-unit margin), filtered by the density map's R channel if set, and
/// snapped to terrain height. Each blade gets a random Y rotation and per-blade scale variation
/// (±size_variation). Reallocates the transform/position buffers and resets `total_count`.
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param terrain Terrain3D handle supplying validated extents and sampled heights.
/// @param count Maximum candidate blade count; zero clears the current population.
void rt_vegetation3d_populate(void *obj, void *terrain, int64_t count) {
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v)
        return;
    vegetation3d_repair_resource_handles(v);
    if (count <= 0) {
        vegetation3d_clear_population_buffers(v);
        return;
    }
    if (count > VEGETATION3D_MAX_BLADES) {
        rt_trap("Vegetation3D.Populate: count exceeds supported range");
        return;
    }

    /* Copy validated metadata through Terrain3D's opaque internal contract. */
    rt_terrain3d_grid_info terrain_info;
    if (!rt_terrain3d_get_grid_info_internal(terrain, &terrain_info))
        return;

    double tw = terrain_info.extent_x;
    double td = terrain_info.extent_z;
    if (tw > VEGETATION3D_TERRAIN_EXTENT_MAX)
        tw = VEGETATION3D_TERRAIN_EXTENT_MAX;
    if (td > VEGETATION3D_TERRAIN_EXTENT_MAX)
        td = VEGETATION3D_TERRAIN_EXTENT_MAX;
    if (!isfinite(tw) || !isfinite(td) || tw <= 0.0 || td <= 0.0) {
        rt_trap("Vegetation3D.Populate: invalid terrain extents");
        return;
    }

    /* Allocate storage */
    int32_t cap = (int32_t)count;
    size_t base_count;
    size_t pos_count;
    size_t base_bytes;
    size_t pos_bytes;
    if (!rt_world3d_checked_mul_size((size_t)cap, 16u, &base_count) ||
        !rt_world3d_checked_mul_size((size_t)cap, 3u, &pos_count) ||
        !rt_world3d_checked_mul_size(base_count, sizeof(float), &base_bytes) ||
        !rt_world3d_checked_mul_size(pos_count, sizeof(float), &pos_bytes)) {
        rt_trap("Vegetation3D.Populate: allocation size overflow");
        return;
    }
    float *new_base_transforms = (float *)calloc(1, base_bytes);
    float *new_positions = (float *)calloc(1, pos_bytes);
    if (!new_base_transforms || !new_positions) {
        free(new_base_transforms);
        free(new_positions);
        rt_trap("Vegetation3D.Populate: allocation failed");
        return;
    }

    rt_pixels_impl *density_map = NULL;
    if (v->density_map) {
        density_map = rt_pixels_checked_impl_or_null(v->density_map);
        if (!density_map || density_map->width <= 0 || density_map->height <= 0 ||
            !density_map->data) {
            free(new_base_transforms);
            free(new_positions);
            rt_trap("Vegetation3D.Populate: density map is invalid");
            return;
        }
    }

    free(v->base_transforms);
    free(v->positions);
    v->base_transforms = new_base_transforms;
    v->positions = new_positions;
    v->capacity = cap;
    v->total_count = 0;
    v->visible_count = 0;
    v->grid_ready = 0;

    uint32_t rng = v->scatter_seed ? v->scatter_seed : vegetation3d_seed_from_i64(42);
    double min_x = tw >= 4.0 ? 2.0 : 0.0;
    double max_x = tw >= 4.0 ? tw - 2.0 : tw;
    double min_z = td >= 4.0 ? 2.0 : 0.0;
    double max_z = td >= 4.0 ? td - 2.0 : td;

    for (int64_t i = 0; i < count; i++) {
        /* Random position within terrain bounds (margin of 2 units) */
        double fx = (double)(lcg_next(&rng) & 0xFFFF) / 65535.0;
        double fz = (double)(lcg_next(&rng) & 0xFFFF) / 65535.0;
        double wx = min_x + fx * (max_x - min_x);
        double wz = min_z + fz * (max_z - min_z);

        /* Density map check */
        if (density_map) {
            int64_t px = (int64_t)(fx * (double)(density_map->width - 1));
            int64_t pz = (int64_t)(fz * (double)(density_map->height - 1));
            if (px < 0)
                px = 0;
            if (pz < 0)
                pz = 0;
            if (px >= density_map->width)
                px = density_map->width - 1;
            if (pz >= density_map->height)
                pz = density_map->height - 1;
            uint32_t pixel = density_map->data[pz * density_map->width + px];
            int32_t density = (int32_t)((pixel >> 24) & 0xFF); /* R channel */
            uint32_t roll = lcg_next(&rng) & 0xFF;
            if (density <= 0)
                continue;
            if (density < 255 && (int32_t)roll >= density)
                continue; /* skip based on density */
        }

        double wy = rt_terrain3d_get_height_at(terrain, wx, wz);
        wy = vegetation_abs_or(wy, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);

        /* Random Y rotation + scale variation */
        double angle = ((double)(lcg_next(&rng) & 0xFFFF) / 65535.0) * 6.283185307;
        double scale_var =
            1.0 + (((double)(lcg_next(&rng) & 0xFFFF) / 65535.0) - 0.5) * 2.0 * v->size_variation;
        scale_var = vegetation_positive_or(scale_var, 1.0, VEGETATION3D_BLADE_DIM_MAX);
        if (scale_var < 0.01)
            scale_var = 0.01;

        int32_t idx = v->total_count;
        build_transform(&v->base_transforms[idx * 16], wx, wy, wz, angle, scale_var);
        v->positions[idx * 3 + 0] = (float)wx;
        v->positions[idx * 3 + 1] = (float)wy;
        v->positions[idx * 3 + 2] = (float)wz;
        v->total_count++;
    }
}

/// @brief Rebuild the CSR spatial grid that buckets blade indices by XZ cell.
/// @details Sized so a cell holds ~128 blades on average (clamped to 4..128 cells per axis).
///   Blades with non-finite positions are left out of every cell; they were already invisible to
///   the culling pass. Returns 0 (grid unavailable, callers fall back to the linear walk) on
///   allocation failure or degenerate extents.
/// @param v Vegetation system whose current population is bucketed.
/// @return 1 when a usable grid is installed, otherwise 0.
static int vegetation3d_rebuild_grid(rt_vegetation3d *v) {
    vegetation3d_free_grid(v);
    if (!v || v->total_count <= 0 || !v->positions)
        return 0;

    float min_x = 0.0f, min_z = 0.0f, max_x = 0.0f, max_z = 0.0f;
    int have_bounds = 0;
    for (int32_t i = 0; i < v->total_count; i++) {
        float bx = v->positions[i * 3 + 0];
        float bz = v->positions[i * 3 + 2];
        if (!isfinite(bx) || !isfinite(bz))
            continue;
        if (!have_bounds) {
            min_x = max_x = bx;
            min_z = max_z = bz;
            have_bounds = 1;
            continue;
        }
        if (bx < min_x)
            min_x = bx;
        if (bx > max_x)
            max_x = bx;
        if (bz < min_z)
            min_z = bz;
        if (bz > max_z)
            max_z = bz;
    }
    if (!have_bounds)
        return 0;

    int32_t cells = (int32_t)ceil(sqrt((double)v->total_count / 128.0));
    if (cells < 4)
        cells = 4;
    if (cells > VEGETATION3D_GRID_MAX_CELLS)
        cells = VEGETATION3D_GRID_MAX_CELLS;
    float extent_x = max_x - min_x;
    float extent_z = max_z - min_z;
    if (extent_x < 1e-3f)
        extent_x = 1e-3f;
    if (extent_z < 1e-3f)
        extent_z = 1e-3f;

    int32_t cell_count = cells * cells;
    int32_t *starts = (int32_t *)calloc((size_t)cell_count + 1u, sizeof(int32_t));
    int32_t *indices = (int32_t *)malloc((size_t)v->total_count * sizeof(int32_t));
    if (!starts || !indices) {
        free(starts);
        free(indices);
        return 0;
    }

    v->grid_cells_x = cells;
    v->grid_cells_z = cells;
    v->grid_min_x = min_x;
    v->grid_min_z = min_z;
    v->grid_cell_w = extent_x / (float)cells;
    v->grid_cell_d = extent_z / (float)cells;

    /* Counting sort: pass 1 counts, prefix-sum, pass 2 scatters. */
    for (int32_t i = 0; i < v->total_count; i++) {
        float bx = v->positions[i * 3 + 0];
        float bz = v->positions[i * 3 + 2];
        if (!isfinite(bx) || !isfinite(bz))
            continue;
        int32_t cx = (int32_t)((bx - min_x) / v->grid_cell_w);
        int32_t cz = (int32_t)((bz - min_z) / v->grid_cell_d);
        if (cx < 0)
            cx = 0;
        if (cx >= cells)
            cx = cells - 1;
        if (cz < 0)
            cz = 0;
        if (cz >= cells)
            cz = cells - 1;
        starts[cz * cells + cx + 1]++;
    }
    for (int32_t c = 0; c < cell_count; c++)
        starts[c + 1] += starts[c];
    int32_t *cursor = (int32_t *)malloc((size_t)cell_count * sizeof(int32_t));
    if (!cursor) {
        free(starts);
        free(indices);
        v->grid_cells_x = 0;
        v->grid_cells_z = 0;
        return 0;
    }
    memcpy(cursor, starts, (size_t)cell_count * sizeof(int32_t));
    for (int32_t i = 0; i < v->total_count; i++) {
        float bx = v->positions[i * 3 + 0];
        float bz = v->positions[i * 3 + 2];
        if (!isfinite(bx) || !isfinite(bz))
            continue;
        int32_t cx = (int32_t)((bx - min_x) / v->grid_cell_w);
        int32_t cz = (int32_t)((bz - min_z) / v->grid_cell_d);
        if (cx < 0)
            cx = 0;
        if (cx >= cells)
            cx = cells - 1;
        if (cz < 0)
            cz = 0;
        if (cz >= cells)
            cz = cells - 1;
        indices[cursor[cz * cells + cx]++] = i;
    }
    free(cursor);

    v->grid_cell_start = starts;
    v->grid_indices = indices;
    v->grid_ready = 1;
    return 1;
}

/// @brief Frame-constant parameters shared by the per-blade culling helper.
typedef struct {
    double camX;
    double camZ;
    double lod_near2;
    double lod_far2;
    float lod_near;
    float lod_far;
    double wind_speed;
    double wind_strength;
    double wind_turbulence;
    double time;
} vegetation3d_update_params;

/// @brief Floor and clamp a normalized grid coordinate before narrowing it to int32.
/// @details Large LOD radii combined with a near-degenerate grid extent can produce coordinates
///   outside the int32 range. Clamping in double precision avoids an undefined narrowing conversion
///   that can otherwise turn an upper cell bound negative and skip the entire culling walk.
/// @param coordinate Continuous cell-space coordinate.
/// @param cell_count Number of cells on the axis.
/// @return Valid cell index in `[0, cell_count - 1]`.
static int32_t vegetation3d_clamp_grid_cell(double coordinate, int32_t cell_count) {
    if (cell_count <= 1 || isnan(coordinate) || coordinate <= 0.0)
        return 0;
    if (!isfinite(coordinate) || coordinate >= (double)cell_count)
        return cell_count - 1;
    return (int32_t)floor(coordinate);
}

/// @brief Cull, thin, fade, and wind-shear one blade, appending it to the visible buffer.
/// @param v Vegetation system containing source and destination transform buffers.
/// @param i Source blade index.
/// @param p Sanitized frame-constant culling and wind parameters.
static void vegetation3d_collect_blade(rt_vegetation3d *v,
                                       int32_t i,
                                       const vegetation3d_update_params *p) {
    if (i < 0 || i >= v->total_count)
        return; /* stale grid entry after a state repair shrank the population */
    float bx = v->positions[i * 3 + 0];
    float bz = v->positions[i * 3 + 2];
    if (!isfinite(bx) || !isfinite(bz))
        return;

    /* Distance to camera (XZ plane) */
    double dx = (double)bx - p->camX;
    double dz = (double)bz - p->camZ;
    double dist2 = dx * dx + dz * dz;
    if (!isfinite(dist2))
        return;
    /* Hard-cull via squared distance so only the transition band pays for sqrt-based thinning.
     * The scale fade below has already shrunk blades to zero by this radius. */
    if (dist2 > p->lod_far2)
        return;

    /* Monotone thinning + scale fade between near and far */
    float fade = 1.0f;
    if (dist2 > p->lod_near2) {
        float dist = (float)sqrt(dist2);
        if (!isfinite(dist))
            return;
        float denom = p->lod_far - p->lod_near;
        float t = denom > 1e-6f ? (dist - p->lod_near) / denom : 1.0f;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        /* Density falls as 1/(1+4t): the same budget the old skip-stride tiers (1/1..1/5)
         * spent, but continuous and monotone — each blade owns a stable random threshold,
         * so the kept set only shrinks as t grows instead of swapping cohorts at edges. */
        float keep = 1.0f / (1.0f + 4.0f * t);
        if (vegetation3d_lod_hash01(i, bx, bz) >= keep)
            return;
        /* Scale-fade the outermost band so survivors shrink out instead of popping at lod_far. */
        if (t > VEGETATION3D_LOD_FADE_START) {
            fade = (1.0f - t) * (1.0f / (1.0f - VEGETATION3D_LOD_FADE_START));
            if (fade < VEGETATION3D_LOD_FADE_FLOOR)
                return;
        }
    }

    /* Copy base transform */
    float *dst = &v->visible_transforms[v->visible_count * 16];
    memcpy(dst, &v->base_transforms[i * 16], 16 * sizeof(float));
    if (!vegetation3d_matrix_is_drawable(dst))
        return;

    /* Apply wind shear to the transform's Y-column entries.
     * This bends the blade tops in the wind direction. */
    double phase = p->wind_turbulence * (bx * 0.1 + bz * 0.07) + p->time * p->wind_speed;
    if (!isfinite(phase))
        phase = 0.0;
    phase = fmod(phase, VEGETATION3D_TWO_PI);
    if (!isfinite(phase))
        phase = 0.0;
    double wave_x = vegetation3d_wind_wave(phase, (double)bx, (double)bz);
    double wave_z =
        vegetation3d_wind_wave(phase * 0.71 + 1.0471975511965976, (double)bz, (double)bx);
    float wind_x = (float)(wave_x * p->wind_strength);
    float wind_z = (float)(wave_z * p->wind_strength * 0.42);
    if (!isfinite(wind_x))
        wind_x = 0.0f;
    if (!isfinite(wind_z))
        wind_z = 0.0f;
    /* Bend the blade's up (Y) axis toward the wind. In this row-major 4x4,
     * indices 1 and 9 are the X and Z components of the second (Y) column,
     * so adding to them leans the blade top along world X and Z. */
    dst[1] = (float)vegetation_abs_or(
        (double)dst[1] + (double)wind_x, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    dst[9] = (float)vegetation_abs_or(
        (double)dst[9] + (double)wind_z, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    if (fade < 1.0f) {
        /* Uniformly shrink the 3x3 basis (translation untouched) for the far fade-out. */
        dst[0] *= fade;
        dst[1] *= fade;
        dst[2] *= fade;
        dst[4] *= fade;
        dst[5] *= fade;
        dst[6] *= fade;
        dst[8] *= fade;
        dst[9] *= fade;
        dst[10] *= fade;
    }
    if (!vegetation3d_matrix_is_drawable(dst))
        return;

    v->visible_count++;
}

/// @brief Per-frame tick. Advances wind time by `dt`, then rebuilds the visible-instances buffer:
/// hard-cull beyond `lod_far`, monotone per-blade thinning between `lod_near` and `lod_far` with a
/// scale fade-out approaching the far edge, and per-blade wind shear on columns 1 and 9 of the
/// transform (bending blade tops). Culling iterates only the spatial-grid cells within `lod_far`
/// of the camera, so per-frame cost scales with the visible set, not the total population.
/// `camY` is unused — culling is XZ-only.
/// @param obj Vegetation3D handle; invalid handles are ignored.
/// @param dt Nonnegative finite simulation delta, clamped to one second.
/// @param camX Camera world-space x coordinate.
/// @param camY Camera world-space y coordinate; currently unused.
/// @param camZ Camera world-space z coordinate.
void rt_vegetation3d_update(void *obj, double dt, double camX, double camY, double camZ) {
    (void)camY;
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!v || !isfinite(dt) || dt < 0.0)
        return;
    if (!vegetation3d_repair_state(v))
        return;
    if (dt > VEGETATION3D_DT_MAX)
        dt = VEGETATION3D_DT_MAX;
    camX = vegetation_abs_or(camX, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    camZ = vegetation_abs_or(camZ, 0.0, VEGETATION3D_TERRAIN_EXTENT_MAX);
    v->time += dt;
    if (!isfinite(v->time))
        v->time = 0.0;
    if (v->time > VEGETATION3D_TIME_MAX)
        v->time = fmod(v->time, VEGETATION3D_TIME_MAX);

    if (v->total_count <= 0 || !v->base_transforms || !v->positions) {
        v->visible_count = 0;
        return;
    }

    float lod_near =
        (float)vegetation_nonnegative_or((double)v->lod_near, VEGETATION3D_TERRAIN_EXTENT_MAX);
    float lod_far =
        (float)vegetation_nonnegative_or((double)v->lod_far, VEGETATION3D_TERRAIN_EXTENT_MAX);
    if (lod_far <= lod_near)
        lod_far = lod_near + 1.0f;
    const double lod_near2 = (double)lod_near * (double)lod_near;
    const double lod_far2 = (double)lod_far * (double)lod_far;
    double wind_speed = vegetation_abs_or(v->wind_speed, 0.0, VEGETATION3D_WIND_PARAM_MAX);
    double wind_strength = vegetation_nonnegative_or(v->wind_strength, VEGETATION3D_WIND_PARAM_MAX);
    double wind_turbulence =
        vegetation_nonnegative_or(v->wind_turbulence, VEGETATION3D_WIND_PARAM_MAX);

    /* Ensure visible buffer is large enough */
    if (v->visible_capacity < v->total_count) {
        size_t visible_count;
        size_t visible_bytes;
        if (!rt_world3d_checked_mul_size((size_t)v->total_count, 16u, &visible_count) ||
            !rt_world3d_checked_mul_size(visible_count, sizeof(float), &visible_bytes)) {
            v->visible_count = 0;
            rt_trap("Vegetation3D.Update: visible buffer size overflow");
            return;
        }
        float *new_visible = (float *)realloc(v->visible_transforms, visible_bytes);
        if (!new_visible) {
            v->visible_count = 0;
            rt_trap("Vegetation3D.Update: visible buffer allocation failed");
            return;
        }
        v->visible_transforms = new_visible;
        v->visible_capacity = v->total_count;
    }
    v->visible_count = 0;

    vegetation3d_update_params params;
    params.camX = camX;
    params.camZ = camZ;
    params.lod_near2 = lod_near2;
    params.lod_far2 = lod_far2;
    params.lod_near = lod_near;
    params.lod_far = lod_far;
    params.wind_speed = wind_speed;
    params.wind_strength = wind_strength;
    params.wind_turbulence = wind_turbulence;
    params.time = v->time;

    if (!v->grid_ready)
        vegetation3d_rebuild_grid(v);

    if (v->grid_ready && v->grid_cell_start && v->grid_indices && v->grid_cells_x > 0 &&
        v->grid_cells_z > 0) {
        /* Walk only the grid cells whose AABB intersects the lod_far disc around the camera. */
        int32_t cells_x = v->grid_cells_x;
        int32_t cells_z = v->grid_cells_z;
        double inv_w = 1.0 / (double)v->grid_cell_w;
        double inv_d = 1.0 / (double)v->grid_cell_d;
        int32_t cx0 = vegetation3d_clamp_grid_cell(
            (camX - (double)lod_far - (double)v->grid_min_x) * inv_w, cells_x);
        int32_t cx1 = vegetation3d_clamp_grid_cell(
            (camX + (double)lod_far - (double)v->grid_min_x) * inv_w, cells_x);
        int32_t cz0 = vegetation3d_clamp_grid_cell(
            (camZ - (double)lod_far - (double)v->grid_min_z) * inv_d, cells_z);
        int32_t cz1 = vegetation3d_clamp_grid_cell(
            (camZ + (double)lod_far - (double)v->grid_min_z) * inv_d, cells_z);
        for (int32_t cz = cz0; cz <= cz1; cz++) {
            double cell_min_z = (double)v->grid_min_z + (double)cz * (double)v->grid_cell_d;
            double cell_max_z = cell_min_z + (double)v->grid_cell_d;
            double ndz = camZ < cell_min_z ? cell_min_z - camZ
                                           : (camZ > cell_max_z ? camZ - cell_max_z : 0.0);
            for (int32_t cx = cx0; cx <= cx1; cx++) {
                double cell_min_x = (double)v->grid_min_x + (double)cx * (double)v->grid_cell_w;
                double cell_max_x = cell_min_x + (double)v->grid_cell_w;
                double ndx = camX < cell_min_x ? cell_min_x - camX
                                               : (camX > cell_max_x ? camX - cell_max_x : 0.0);
                if (ndx * ndx + ndz * ndz > lod_far2)
                    continue; /* corner cell of the bounding square, outside the disc */
                int32_t c = cz * cells_x + cx;
                int32_t begin = v->grid_cell_start[c];
                int32_t end = v->grid_cell_start[c + 1];
                for (int32_t k = begin; k < end; k++)
                    vegetation3d_collect_blade(v, v->grid_indices[k], &params);
            }
        }
    } else {
        /* Grid unavailable (allocation failure): fall back to the full linear walk. */
        for (int32_t i = 0; i < v->total_count; i++)
            vegetation3d_collect_blade(v, i, &params);
    }
}

/// @brief Submit the visible blade batch to the Canvas3D as one instanced draw.
/// @details Marks the blade material double-sided so grass renders from both sides without
///          mutating canvas-level culling state. No-op when called outside a frame, when the
///          backend is missing, or when nothing is visible.
/// @param canvas_obj Canvas3D handle receiving the instanced batch.
/// @param veg_obj Vegetation3D handle supplying shared geometry and visible transforms.
void rt_canvas3d_draw_vegetation(void *canvas_obj, void *veg_obj) {
    if (!canvas_obj || !veg_obj)
        return;
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas_obj);
    rt_vegetation3d *v =
        (rt_vegetation3d *)rt_g3d_checked_or_null(veg_obj, RT_G3D_VEGETATION3D_CLASS_ID);
    if (!c || !v)
        return;
    if (!vegetation3d_repair_state(v))
        return;
    if (!c->in_frame || !c->backend || v->visible_count <= 0)
        return;
    if (c->frame_is_2d) {
        rt_trap("Canvas3D.DrawVegetation: cannot draw vegetation during Begin2D/End");
        return;
    }

    rt_mesh3d *mesh = vegetation3d_mesh_ref(v->blade_mesh);
    rt_material3d *mat = vegetation3d_material_ref(v->blade_material);
    vegetation3d_compact_visible(v);
    if (!mesh || mesh->vertex_count == 0 || !mat || !v->visible_transforms ||
        v->visible_count <= 0 || v->visible_count > v->visible_capacity)
        return;

    vegetation3d_enable_double_sided_material(mat);
    rt_mesh3d_refresh_bounds(mesh);
    rt_canvas3d_queue_instanced_batch_bounds(c,
                                             mesh,
                                             mat,
                                             v->visible_transforms,
                                             v->visible_count,
                                             NULL,
                                             0,
                                             mesh->aabb_min,
                                             mesh->aabb_max,
                                             0,
                                             1);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
