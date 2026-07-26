//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_terrain3d.c
// Purpose: Heightmap terrain — chunked mesh generation, bilinear height/normal
//   queries, frustum-culled per-chunk rendering.
//
// Key invariants:
//   - Heights are float[width*depth], sampled from Pixels red channel.
//   - Chunks are TERRAIN_CHUNK_SIZE quads per edge (16x16 = 256 quads each).
//   - Mesh generation is lazy (built on first draw, invalidated on heightmap change).
//   - Normals computed via central difference on height grid.
//   - Three LOD levels keyed by camera distance (full / step=2 / step=4).
//   - Splat-map composites up to 4 layer textures into a baked diffuse.
//
// Ownership/Lifetime:
//   - Terrain3D is GC-managed; finalizer releases all per-chunk LOD meshes,
//     the material, splat map, layer textures, and base + baked textures.
//
// Links: rt_terrain3d.h, rt_mesh3d, vgfx3d_frustum.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_terrain3d.c
 * @brief Defines Terrain3D private state and its validated cross-module grid descriptor.
 *
 * Lifecycle, construction, LOD, and drawing routines are split into order-sensitive implementation
 * includes. The private payload owns sampled heights, per-chunk meshes and bounds, splat resources,
 * hole masks, and draw telemetry.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_terrain3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_pixels_internal.h"
#include "rt_platform.h"
#include "rt_terrain3d_internal.h"
#include "rt_world3d_common.h"

#include "vgfx3d_frustum.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern void *rt_vec3_new(double x, double y, double z);
extern void *rt_mesh3d_new(void);
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
                                                      float culling_pad);
extern void rt_material3d_set_texture(void *material, void *pixels);
extern void *rt_pixels_new(int64_t width, int64_t height);
extern void rt_pixels_set(void *pixels, int64_t x, int64_t y, int64_t color);

#define TERRAIN_CHUNK_SIZE 16
#define TERRAIN3D_ABS_MAX 1000000.0
#define TERRAIN3D_MAX_DIM 4096
#define TERRAIN3D_MAX_CHUNKS 65536

#define TERRAIN_MAX_SPLAT_LAYERS 8
#define TERRAIN_LOD_LEVELS 3

#if RT_COMPILER_GCC_LIKE
#define TERRAIN3D_UNUSED_PRIVATE __attribute__((unused))
#else
#define TERRAIN3D_UNUSED_PRIVATE
#endif

/**
 * @struct rt_terrain3d
 * @brief Private state for a chunked, heightmap-backed terrain runtime object.
 *
 * Mesh and AABB arrays are indexed by chunk, with separate caches for each supported LOD. Retained
 * texture/material handles, authored hole data, and generated splat resources share the lifetime
 * of the GC-managed terrain object.
 */
typedef struct {
    void *vptr;
    float *heights;
    int32_t width, depth;
    int64_t height_count;
    double scale[3];          /* x_spacing, y_scale, z_spacing */
    void **chunk_meshes;      /* LOD 0 (full res) mesh cache */
    void **chunk_meshes_lod1; /* LOD 1 (step=2) mesh cache */
    void **chunk_meshes_lod2; /* LOD 2 (step=4) mesh cache */
    float *chunk_aabbs;       /* 6 floats per chunk: min[3], max[3] */
    uint8_t *chunk_lod_state; /* last selected LOD per chunk; 0xFF = uninitialized */
    int32_t chunks_x, chunks_z;
    int32_t chunk_capacity;
    void *material;
    /* LOD distance thresholds */
    float lod_dist1;      /* distance beyond which LOD 1 is used */
    float lod_dist2;      /* distance beyond which LOD 2 is used */
    float lod_hysteresis; /* distance band that prevents LOD threshold flicker */
    float skirt_depth;    /* depth of crack-hiding skirts (0 = disabled) */
    /* Splat maps: RGBA Pixels; map 0 weights layers 0-3, map 1 weights layers 4-7 */
    void *splat_map;
    void *splat_map2;
    void *layer_textures[TERRAIN_MAX_SPLAT_LAYERS];
    double layer_scales[TERRAIN_MAX_SPLAT_LAYERS]; /* UV tiling per layer */
    void *base_texture;
    void *baked_texture;
    int8_t splat_dirty;

    /* Procedural slope/height splat rules (RebuildSplatWeights) */
    struct {
        int8_t mode; /* 0 = none, 1 = slope band (degrees), 2 = height band (world Y) */
        double min_value;
        double max_value;
        double sharpness;
    } layer_rules[TERRAIN_MAX_SPLAT_LAYERS];

    /* Hole carving: authored XZ rects (terrain-local units) rasterized to a
     * per-cell bitmask consumed by chunk builds, nav meshes, and (via the raw
     * mask getter) heightfield colliders. */
    double (*holes)[4]; /* x, z, width, depth per hole */
    int32_t hole_count;
    int32_t hole_capacity;
    uint8_t *hole_mask; /* bitmask over (width-1)*(depth-1) cells, or NULL */
    int8_t holes_dirty;
    int8_t lod_prewarm_dirty; /* true when missing chunk LOD meshes should be rebuilt */
    float *chunk_aabbs_lod1;  /* 6 floats per chunk for LOD 1 generated geometry */
    float *chunk_aabbs_lod2;  /* 6 floats per chunk for LOD 2 generated geometry */
    float *chunk_aabbs_union; /* conservative union of every resident LOD for stable culling */
    int32_t last_chunk_count;
    int32_t last_drawn_chunk_count;
    int32_t last_frustum_culled_chunk_count;
    int32_t last_missing_lod_count;
    int32_t last_lod_counts[TERRAIN_LOD_LEVELS];
    int32_t last_lod_clamped_chunk_count;
    int8_t cpu_occlusion_enabled; /* opt-in: default off because terrain AABBs are not solid */
} rt_terrain3d;

// clang-format off
// Order-sensitive implementation includes: lifecycle/LOD helpers are used by
// construction, and draw uses the construction helpers.
#include "rt_terrain3d_lod.inc"
#include "rt_terrain3d_build.inc"
#include "rt_terrain3d_draw.inc"
// clang-format on

/// @brief Copy validated Terrain3D dimensions, scale, and cell-derived extents.
/// @param obj Borrowed opaque Terrain3D handle.
/// @param out_info Caller-owned destination; always zeroed before validation.
/// @return `1` when the terrain grid and all copied scalar values are valid,
///   otherwise `0` with a zero descriptor.
/// @details This is the sole cross-module metadata bridge for code such as
///   Vegetation3D. Keeping the copy here prevents sibling modules and isolated
///   tests from mirroring the private `rt_terrain3d` payload. Extents count
///   intervals between samples and therefore use `(dimension - 1) * spacing`.
int8_t rt_terrain3d_get_grid_info_internal(void *obj, rt_terrain3d_grid_info *out_info) {
    rt_terrain3d *terrain;
    double extent_x;
    double extent_z;

    if (!out_info)
        return 0;
    memset(out_info, 0, sizeof(*out_info));

    terrain = (rt_terrain3d *)rt_g3d_checked_or_null(obj, RT_G3D_TERRAIN3D_CLASS_ID);
    if (!terrain || !terrain_has_valid_grid(terrain) || !isfinite(terrain->scale[0]) ||
        terrain->scale[0] <= 0.0 || !isfinite(terrain->scale[1]) || !isfinite(terrain->scale[2]) ||
        terrain->scale[2] <= 0.0)
        return 0;

    extent_x = (double)(terrain->width - 1) * terrain->scale[0];
    extent_z = (double)(terrain->depth - 1) * terrain->scale[2];
    if (!isfinite(extent_x) || extent_x <= 0.0 || !isfinite(extent_z) || extent_z <= 0.0)
        return 0;

    out_info->width = terrain->width;
    out_info->depth = terrain->depth;
    out_info->spacing_x = terrain->scale[0];
    out_info->height_scale = terrain->scale[1];
    out_info->spacing_z = terrain->scale[2];
    out_info->extent_x = extent_x;
    out_info->extent_z = extent_z;
    return 1;
}
#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
