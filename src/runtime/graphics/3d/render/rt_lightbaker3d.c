//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_lightbaker3d.c
// Purpose: Baked global illumination — LightBaker3D (deterministic CPU path
//   tracer over static scene geometry producing a lightmap atlas with
//   per-triangle charts) and LightProbeGrid3D (SH-9 irradiance probe grid for
//   dynamic objects), with .vlm/.vlpg serialization.
// Key invariants:
//   - Bakes are deterministic: fixed per-texel/per-probe sample seeds, no
//     wall-clock or thread-order dependence (single-threaded chunked BakeStep).
//   - Charts write TEXCOORD_1 directly into mesh vertices; the atlas applies
//     through Material3D lightmap slots on per-node material instances.
// Ownership/Lifetime:
//   - Baker/grid are GC-managed; the baker retains its scene and output atlas.
//   - Explicit light state is copied at AddLight time rather than retained.
//   - Bakers and grids own their native BVH, atlas, validity, and SH arrays.
// Links: misc/plans/thirdpersonupgrade/14-baked-gi.md, docs/adr/0088.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic lightmap baking and SH-9 irradiance probe grids.
/// @details Static scene geometry is flattened into a triangle BVH, sampled by
///   deterministic direct/indirect radiance estimators, packed into a lightmap
///   atlas, and reused to bake, sample, save, and load spatial probe grids.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_lightbaker3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_pixels_internal.h"
#include "rt_scene3d.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern double rt_vec3_x(void *v);
extern double rt_vec3_y(void *v);
extern double rt_vec3_z(void *v);

/// @brief Read a Vec3's components; returns 0 for a non-Vec3 handle.
/// @param v Candidate Vec3 runtime handle.
/// @param x Output X component.
/// @param y Output Y component.
/// @param z Output Z component.
/// @return Nonzero when @p v has the Vec3 class and all outputs were written.
static int baker_read_vec3(void *v, double *x, double *y, double *z) {
    if (!v || rt_obj_class_id(v) != RT_VEC3_CLASS_ID)
        return 0;
    *x = rt_vec3_x(v);
    *y = rt_vec3_y(v);
    *z = rt_vec3_z(v);
    return 1;
}

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern int64_t rt_obj_class_id(void *obj);
extern void *rt_pixels_new(int64_t width, int64_t height);
extern void rt_pixels_set(void *pixels, int64_t x, int64_t y, int64_t color);
extern int64_t rt_pixels_get(void *pixels, int64_t x, int64_t y);
extern int64_t rt_pixels_width(void *pixels);
extern int64_t rt_pixels_height(void *pixels);

#define BAKER3D_MAX_TRIS 262144
#define BAKER3D_MAX_NODES 4096
#define BAKER3D_ATLAS_DIM 1024
#define BAKER3D_EPS 1e-4
#define BAKER3D_MAX_LIGHTS 16

/*==========================================================================
 * Deterministic sampler (per-texel seeded LCG)
 *=========================================================================*/

/// @brief Advance the deterministic per-sample linear congruential generator.
/// @param state In/out 32-bit generator state.
/// @return Newly advanced raw generator value.
static uint32_t baker_lcg_next(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

/// @brief Draw one deterministic uniform scalar from the baker generator.
/// @param state In/out 32-bit generator state.
/// @return Value in `[0, 1)` derived from the upper 24 random bits.
static double baker_lcg_unit(uint32_t *state) {
    return (double)(baker_lcg_next(state) >> 8) / 16777216.0;
}

/*==========================================================================
 * Triangle soup + BVH
 *=========================================================================*/

typedef struct baker_tri {
    double p0[3], p1[3], p2[3];
    double normal[3];
    double albedo[3];
    double emissive[3];
    int32_t node_index; /* source baked-node index */
    int32_t tri_index;  /* triangle index within the source mesh */
} baker_tri;

typedef struct baker_bvh_node {
    double min_b[3], max_b[3];
    int32_t left;  /* child index, or -1 for leaf */
    int32_t right; /* child index, or -1 for leaf */
    int32_t first; /* leaf: first tri index in order array */
    int32_t count; /* leaf: tri count */
} baker_bvh_node;

typedef struct baker_node_entry {
    void *node;     /* borrowed scene node (scene retained by baker) */
    void *mesh;     /* borrowed mesh */
    void *material; /* borrowed material (may be NULL) */
    int32_t first_tri;
    int32_t tri_count;
} baker_node_entry;

typedef struct baker_light {
    int32_t type; /* 0 directional, 1 point, 3 spot (ambient skipped) */
    double direction[3];
    double position[3];
    double color[3];
    double intensity;
    double attenuation;
} baker_light;

typedef struct rt_lightbaker3d {
    void *vptr;
    void *scene; /* retained */
    double texels_per_unit;
    int64_t samples;
    int64_t bounces;
    double sky_color[3];
    double progress;
    int8_t done;
    /* gathered scene */
    baker_tri *tris;
    int32_t tri_count;
    int32_t *bvh_order;
    baker_bvh_node *bvh_nodes;
    int32_t bvh_node_count;
    baker_node_entry nodes[BAKER3D_MAX_NODES];
    int32_t node_count;
    baker_light lights[BAKER3D_MAX_LIGHTS];
    int32_t light_count;
    /* atlas + charts */
    void *atlas;             /* retained Pixels */
    float *atlas_hdr;        /* rgb accumulation at atlas resolution */
    uint8_t *atlas_coverage; /* 1 where a chart texel wrote */
    int32_t atlas_dim;
    int32_t cursor_x, cursor_y, row_height; /* shelf packer state */
    int32_t next_tri;                       /* BakeStep cursor over gathered triangles */
    int8_t gathered;
} rt_lightbaker3d;

/*==========================================================================
 * Gathering static geometry
 *=========================================================================*/

/// @brief Snapshot bake-relevant albedo and emissive color from a material.
/// @details Defaults to white diffuse and black emission, releases the fresh
///   color Vec3 returned by the public accessor, and reads emissive state only
///   from a validated Material3D implementation.
/// @param material Borrowed optional Material3D handle.
/// @param albedo Output three-element linear diffuse color.
/// @param emissive Output three-element intensity-scaled emission color.
static void baker_read_material_colors(void *material, double albedo[3], double emissive[3]) {
    albedo[0] = albedo[1] = albedo[2] = 1.0;
    emissive[0] = emissive[1] = emissive[2] = 0.0;
    if (!material)
        return;
    void *color = rt_material3d_get_color(material);
    if (color) {
        double rgb[3];
        if (baker_read_vec3(color, &rgb[0], &rgb[1], &rgb[2])) {
            albedo[0] = rgb[0];
            albedo[1] = rgb[1];
            albedo[2] = rgb[2];
        }
        if (rt_obj_release_check0(color))
            rt_obj_free(color);
    }
    rt_material3d *mat =
        (rt_material3d *)rt_g3d_checked_or_null(material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (mat) {
        emissive[0] = mat->emissive[0] * mat->emissive_intensity;
        emissive[1] = mat->emissive[1] * mat->emissive_intensity;
        emissive[2] = mat->emissive[2] * mat->emissive_intensity;
    }
}

/// @brief Flatten static scene meshes into deterministic world-space triangle soup.
/// @details Traverses a bounded explicit stack, records up to the configured node
///   and triangle caps, copies transformed positions/material colors, repairs
///   degenerate normals, and preserves source node/triangle indices for chart UVs.
///   A previously completed gather is reused.
/// @param baker Baker retaining the source scene and owning gathered arrays.
/// @return Nonzero when at least one triangle is available; zero for missing
///   roots, allocation failure, or an empty eligible scene.
static int baker_gather_scene(rt_lightbaker3d *baker) {
    if (baker->gathered)
        return 1;
    void *root = rt_scene3d_get_root(baker->scene);
    if (!root)
        return 0;
    void *stack[512];
    int32_t stack_count = 0;
    stack[stack_count++] = root;
    int32_t tri_capacity = 4096;
    baker->tris = (baker_tri *)malloc((size_t)tri_capacity * sizeof(baker_tri));
    if (!baker->tris)
        return 0;
    baker->tri_count = 0;
    baker->node_count = 0;
    while (stack_count > 0) {
        void *node = stack[--stack_count];
        int64_t child_count = rt_scene_node3d_child_count(node);
        for (int64_t i = 0; i < child_count && stack_count < 512; ++i) {
            void *child = rt_scene_node3d_get_child(node, i);
            if (child)
                stack[stack_count++] = child;
        }
        if (!rt_scene_node3d_get_static(node))
            continue;
        void *mesh = rt_scene_node3d_get_mesh(node);
        if (!mesh || rt_mesh3d_get_triangle_count(mesh) <= 0)
            continue;
        if (baker->node_count >= BAKER3D_MAX_NODES)
            break;
        double matrix[16];
        if (!rt_scene_node3d_get_world_matrix_components(node, matrix))
            continue;
        void *material = rt_scene_node3d_get_material(node);
        double albedo[3], emissive[3];
        baker_read_material_colors(material, albedo, emissive);
        baker_node_entry *entry = &baker->nodes[baker->node_count];
        entry->node = node;
        entry->mesh = mesh;
        entry->material = material;
        entry->first_tri = baker->tri_count;
        entry->tri_count = 0;
        int64_t tri_count = rt_mesh3d_get_triangle_count(mesh);
        for (int64_t t = 0; t < tri_count; ++t) {
            if (baker->tri_count >= BAKER3D_MAX_TRIS)
                break;
            int64_t idx[3];
            if (!rt_mesh3d_get_triangle_raw(mesh, t, idx))
                break;
            double corners[3][3];
            double authored_normal[3] = {0.0, 0.0, 0.0};
            int ok = 1;
            for (int k = 0; k < 3 && ok; ++k) {
                double pos[3];
                double vn[3];
                if (!rt_mesh3d_get_vertex_raw(mesh, idx[k], pos, vn, NULL)) {
                    ok = 0;
                    break;
                }
                authored_normal[0] += vn[0];
                authored_normal[1] += vn[1];
                authored_normal[2] += vn[2];
                corners[k][0] =
                    matrix[0] * pos[0] + matrix[1] * pos[1] + matrix[2] * pos[2] + matrix[3];
                corners[k][1] =
                    matrix[4] * pos[0] + matrix[5] * pos[1] + matrix[6] * pos[2] + matrix[7];
                corners[k][2] =
                    matrix[8] * pos[0] + matrix[9] * pos[1] + matrix[10] * pos[2] + matrix[11];
            }
            if (!ok)
                break;
            if (baker->tri_count >= tri_capacity) {
                tri_capacity *= 2;
                baker_tri *grown =
                    (baker_tri *)realloc(baker->tris, (size_t)tri_capacity * sizeof(baker_tri));
                if (!grown)
                    return 0;
                baker->tris = grown;
            }
            baker_tri *tri = &baker->tris[baker->tri_count];
            memcpy(tri->p0, corners[0], sizeof(tri->p0));
            memcpy(tri->p1, corners[1], sizeof(tri->p1));
            memcpy(tri->p2, corners[2], sizeof(tri->p2));
            double e1[3] = {corners[1][0] - corners[0][0],
                            corners[1][1] - corners[0][1],
                            corners[1][2] - corners[0][2]};
            double e2[3] = {corners[2][0] - corners[0][0],
                            corners[2][1] - corners[0][1],
                            corners[2][2] - corners[0][2]};
            tri->normal[0] = e1[1] * e2[2] - e1[2] * e2[1];
            tri->normal[1] = e1[2] * e2[0] - e1[0] * e2[2];
            tri->normal[2] = e1[0] * e2[1] - e1[1] * e2[0];
            double len = sqrt(tri->normal[0] * tri->normal[0] + tri->normal[1] * tri->normal[1] +
                              tri->normal[2] * tri->normal[2]);
            if (len > 1e-12) {
                tri->normal[0] /= len;
                tri->normal[1] /= len;
                tri->normal[2] /= len;
            } else {
                tri->normal[0] = 0.0;
                tri->normal[1] = 1.0;
                tri->normal[2] = 0.0;
            }
            /* Orient the geometric normal to agree with the authored vertex
             * normals so winding conventions never flip a bake into darkness. */
            if (tri->normal[0] * authored_normal[0] + tri->normal[1] * authored_normal[1] +
                    tri->normal[2] * authored_normal[2] <
                0.0) {
                tri->normal[0] = -tri->normal[0];
                tri->normal[1] = -tri->normal[1];
                tri->normal[2] = -tri->normal[2];
            }
            memcpy(tri->albedo, albedo, sizeof(tri->albedo));
            memcpy(tri->emissive, emissive, sizeof(tri->emissive));
            tri->node_index = baker->node_count;
            tri->tri_index = (int32_t)t;
            baker->tri_count++;
            entry->tri_count++;
        }
        baker->node_count++;
    }
    baker->gathered = 1;
    return baker->tri_count > 0;
}

/*==========================================================================
 * BVH build (median split) + ray intersection
 *=========================================================================*/

/// @brief Compute an exact axis-aligned bound for one gathered triangle.
/// @param tri Borrowed triangle.
/// @param min_b Output three-element minimum corner.
/// @param max_b Output three-element maximum corner.
static void baker_tri_bounds(const baker_tri *tri, double min_b[3], double max_b[3]) {
    for (int a = 0; a < 3; ++a) {
        double v0 = tri->p0[a], v1 = tri->p1[a], v2 = tri->p2[a];
        min_b[a] = v0 < v1 ? (v0 < v2 ? v0 : v2) : (v1 < v2 ? v1 : v2);
        max_b[a] = v0 > v1 ? (v0 > v2 ? v0 : v2) : (v1 > v2 ? v1 : v2);
    }
}

/// @brief Recursively build a deterministic median-split BVH subtree.
/// @details Bounds the indexed range, emits leaves containing at most four
///   triangles, otherwise insertion-sorts by longest-axis centroid before
///   recursing into two balanced ranges.
/// @param baker Baker owning triangles, order indices, and preallocated nodes.
/// @param first First index in the current order-array range.
/// @param count Positive number of triangles in the range.
/// @return Created node index, or `-1` when node capacity is exhausted.
static int baker_bvh_build_recurse(rt_lightbaker3d *baker, int32_t first, int32_t count) {
    if (baker->bvh_node_count >= baker->tri_count * 2 + 1)
        return -1;
    int32_t node_index = baker->bvh_node_count++;
    baker_bvh_node *node = &baker->bvh_nodes[node_index];
    node->min_b[0] = node->min_b[1] = node->min_b[2] = 1e30;
    node->max_b[0] = node->max_b[1] = node->max_b[2] = -1e30;
    for (int32_t i = first; i < first + count; ++i) {
        double tmin[3], tmax[3];
        baker_tri_bounds(&baker->tris[baker->bvh_order[i]], tmin, tmax);
        for (int a = 0; a < 3; ++a) {
            if (tmin[a] < node->min_b[a])
                node->min_b[a] = tmin[a];
            if (tmax[a] > node->max_b[a])
                node->max_b[a] = tmax[a];
        }
    }
    if (count <= 4) {
        node->left = node->right = -1;
        node->first = first;
        node->count = count;
        return node_index;
    }
    int axis = 0;
    double extent[3] = {node->max_b[0] - node->min_b[0],
                        node->max_b[1] - node->min_b[1],
                        node->max_b[2] - node->min_b[2]};
    if (extent[1] > extent[axis])
        axis = 1;
    if (extent[2] > extent[axis])
        axis = 2;
    /* Median partition by centroid (simple nth_element via insertion — counts are
     * small at the leaves; a full sort keeps determinism trivial). */
    for (int32_t i = first + 1; i < first + count; ++i) {
        int32_t key = baker->bvh_order[i];
        double kc =
            (baker->tris[key].p0[axis] + baker->tris[key].p1[axis] + baker->tris[key].p2[axis]);
        int32_t j = i - 1;
        while (j >= first) {
            int32_t other = baker->bvh_order[j];
            double oc = (baker->tris[other].p0[axis] + baker->tris[other].p1[axis] +
                         baker->tris[other].p2[axis]);
            if (oc <= kc)
                break;
            baker->bvh_order[j + 1] = other;
            j--;
        }
        baker->bvh_order[j + 1] = key;
    }
    int32_t half = count / 2;
    int32_t left = baker_bvh_build_recurse(baker, first, half);
    int32_t right = baker_bvh_build_recurse(baker, first + half, count - half);
    node = &baker->bvh_nodes[node_index]; /* re-fetch: recursion may not realloc, but be safe */
    node->left = left;
    node->right = right;
    node->first = 0;
    node->count = 0;
    return node_index;
}

/// @brief Allocate and build the baker's triangle BVH once.
/// @param baker Baker with a nonempty gathered triangle array.
/// @return Nonzero when an existing or newly rooted BVH is available; zero on
///   allocation or recursive construction failure.
static int baker_bvh_build(rt_lightbaker3d *baker) {
    if (baker->bvh_nodes)
        return 1;
    baker->bvh_order = (int32_t *)malloc((size_t)baker->tri_count * sizeof(int32_t));
    baker->bvh_nodes =
        (baker_bvh_node *)malloc(((size_t)baker->tri_count * 2 + 1) * sizeof(baker_bvh_node));
    if (!baker->bvh_order || !baker->bvh_nodes)
        return 0;
    for (int32_t i = 0; i < baker->tri_count; ++i)
        baker->bvh_order[i] = i;
    baker->bvh_node_count = 0;
    return baker_bvh_build_recurse(baker, 0, baker->tri_count) == 0;
}

/// @brief Test a ray interval against an axis-aligned BVH node.
/// @param origin Three-element ray origin.
/// @param inv_dir Three-element reciprocal ray direction.
/// @param min_b Three-element box minimum.
/// @param max_b Three-element box maximum.
/// @param t_max Exclusive working maximum distance.
/// @return Nonzero when the forward interval intersects before @p t_max.
static int baker_ray_aabb(const double origin[3],
                          const double inv_dir[3],
                          const double min_b[3],
                          const double max_b[3],
                          double t_max) {
    double t0 = 0.0, t1 = t_max;
    for (int a = 0; a < 3; ++a) {
        double near_t = (min_b[a] - origin[a]) * inv_dir[a];
        double far_t = (max_b[a] - origin[a]) * inv_dir[a];
        if (near_t > far_t) {
            double tmp = near_t;
            near_t = far_t;
            far_t = tmp;
        }
        if (near_t > t0)
            t0 = near_t;
        if (far_t < t1)
            t1 = far_t;
        if (t0 > t1)
            return 0;
    }
    return 1;
}

/// @brief Intersect a ray with one two-sided triangle using Moller-Trumbore math.
/// @param origin Three-element ray origin.
/// @param dir Three-element ray direction.
/// @param tri Borrowed triangle.
/// @param t_out Output positive hit distance.
/// @param u_out Output first barycentric coordinate.
/// @param v_out Output second barycentric coordinate.
/// @return Nonzero for a hit beyond the self-intersection epsilon; outputs are
///   written only on success.
static int baker_ray_tri(const double origin[3],
                         const double dir[3],
                         const baker_tri *tri,
                         double *t_out,
                         double *u_out,
                         double *v_out) {
    double e1[3] = {tri->p1[0] - tri->p0[0], tri->p1[1] - tri->p0[1], tri->p1[2] - tri->p0[2]};
    double e2[3] = {tri->p2[0] - tri->p0[0], tri->p2[1] - tri->p0[1], tri->p2[2] - tri->p0[2]};
    double pv[3] = {dir[1] * e2[2] - dir[2] * e2[1],
                    dir[2] * e2[0] - dir[0] * e2[2],
                    dir[0] * e2[1] - dir[1] * e2[0]};
    double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (fabs(det) < 1e-12)
        return 0;
    double inv_det = 1.0 / det;
    double tv[3] = {origin[0] - tri->p0[0], origin[1] - tri->p0[1], origin[2] - tri->p0[2]};
    double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv_det;
    if (u < -1e-9 || u > 1.0 + 1e-9)
        return 0;
    double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1],
                    tv[2] * e1[0] - tv[0] * e1[2],
                    tv[0] * e1[1] - tv[1] * e1[0]};
    double v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv_det;
    if (v < -1e-9 || u + v > 1.0 + 1e-9)
        return 0;
    double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv_det;
    if (t <= BAKER3D_EPS)
        return 0;
    *t_out = t;
    *u_out = u;
    *v_out = v;
    return 1;
}

/// @brief Closest-hit trace. Returns tri index or -1; fills t.
/// @param baker Baker containing a completed BVH and order table.
/// @param origin Three-element ray origin.
/// @param dir Three-element ray direction.
/// @param t_max Maximum search distance.
/// @param t_out Optional output closest distance, written only for a hit.
/// @return Gathered triangle index of the closest hit, or `-1` when none.
static int32_t baker_trace(const rt_lightbaker3d *baker,
                           const double origin[3],
                           const double dir[3],
                           double t_max,
                           double *t_out) {
    if (!baker->bvh_nodes || baker->bvh_node_count <= 0)
        return -1;
    double inv_dir[3];
    for (int a = 0; a < 3; ++a)
        inv_dir[a] = fabs(dir[a]) > 1e-12 ? 1.0 / dir[a] : (dir[a] >= 0.0 ? 1e12 : -1e12);
    int32_t stack[64];
    int32_t stack_count = 0;
    stack[stack_count++] = 0;
    int32_t best = -1;
    double best_t = t_max;
    while (stack_count > 0) {
        const baker_bvh_node *node = &baker->bvh_nodes[stack[--stack_count]];
        if (!baker_ray_aabb(origin, inv_dir, node->min_b, node->max_b, best_t))
            continue;
        if (node->left < 0) {
            for (int32_t i = node->first; i < node->first + node->count; ++i) {
                const baker_tri *tri = &baker->tris[baker->bvh_order[i]];
                double t, u, v;
                if (baker_ray_tri(origin, dir, tri, &t, &u, &v) && t < best_t) {
                    best_t = t;
                    best = baker->bvh_order[i];
                }
            }
        } else if (stack_count + 2 <= 64) {
            stack[stack_count++] = node->left;
            stack[stack_count++] = node->right;
        }
    }
    if (best >= 0 && t_out)
        *t_out = best_t;
    return best;
}

/*==========================================================================
 * Radiance estimation
 *=========================================================================*/

/// @brief Accumulate unshadowed direct irradiance from the baker's copied lights.
/// @details Directional lights use parallel rays; other retained light kinds use
///   position and inverse-quadratic attenuation. BVH shadow rays reject occluded
///   contributions, and negative surface cosines contribute nothing.
/// @param baker Baker containing copied lights and trace geometry.
/// @param point Three-element world-space surface point.
/// @param normal Three-element oriented surface normal.
/// @param out_rgb Output three-element irradiance initialized to black.
static void baker_direct_light(const rt_lightbaker3d *baker,
                               const double point[3],
                               const double normal[3],
                               double out_rgb[3]) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0;
    for (int32_t li = 0; li < baker->light_count; ++li) {
        const baker_light *light = &baker->lights[li];
        double to_light[3];
        double dist = 1e12;
        double atten = 1.0;
        if (light->type == 0) {
            to_light[0] = -light->direction[0];
            to_light[1] = -light->direction[1];
            to_light[2] = -light->direction[2];
        } else {
            to_light[0] = light->position[0] - point[0];
            to_light[1] = light->position[1] - point[1];
            to_light[2] = light->position[2] - point[2];
            dist = sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] +
                        to_light[2] * to_light[2]);
            if (dist < 1e-9)
                continue;
            to_light[0] /= dist;
            to_light[1] /= dist;
            to_light[2] /= dist;
            atten = 1.0 / (1.0 + light->attenuation * dist * dist);
        }
        double ndl = normal[0] * to_light[0] + normal[1] * to_light[1] + normal[2] * to_light[2];
        if (ndl <= 0.0)
            continue;
        double origin[3] = {point[0] + normal[0] * BAKER3D_EPS * 4,
                            point[1] + normal[1] * BAKER3D_EPS * 4,
                            point[2] + normal[2] * BAKER3D_EPS * 4};
        double t;
        if (baker_trace(baker, origin, to_light, dist - BAKER3D_EPS * 8, &t) >= 0)
            continue; /* shadowed */
        double scale = ndl * atten * light->intensity;
        out_rgb[0] += light->color[0] * scale;
        out_rgb[1] += light->color[1] * scale;
        out_rgb[2] += light->color[2] * scale;
    }
}

/// @brief Cosine-weighted hemisphere direction around @p normal.
/// @param rng In/out deterministic sampler state.
/// @param normal Three-element unit surface normal.
/// @param out_dir Output three-element sampled unit direction.
static void baker_cosine_dir(uint32_t *rng, const double normal[3], double out_dir[3]) {
    double r1 = baker_lcg_unit(rng);
    double r2 = baker_lcg_unit(rng);
    double phi = 2.0 * 3.14159265358979323846 * r1;
    double sr2 = sqrt(r2);
    double x = cos(phi) * sr2;
    double y = sin(phi) * sr2;
    double z = sqrt(1.0 - r2 < 0.0 ? 0.0 : 1.0 - r2);
    /* Build a tangent basis around the normal. */
    double up[3] = {0.0, 1.0, 0.0};
    if (fabs(normal[1]) > 0.9) {
        up[0] = 1.0;
        up[1] = 0.0;
    }
    double t0[3] = {up[1] * normal[2] - up[2] * normal[1],
                    up[2] * normal[0] - up[0] * normal[2],
                    up[0] * normal[1] - up[1] * normal[0]};
    double t0_len = sqrt(t0[0] * t0[0] + t0[1] * t0[1] + t0[2] * t0[2]);
    t0[0] /= t0_len;
    t0[1] /= t0_len;
    t0[2] /= t0_len;
    double t1[3] = {normal[1] * t0[2] - normal[2] * t0[1],
                    normal[2] * t0[0] - normal[0] * t0[2],
                    normal[0] * t0[1] - normal[1] * t0[0]};
    out_dir[0] = t0[0] * x + t1[0] * y + normal[0] * z;
    out_dir[1] = t0[1] * x + t1[1] * y + normal[1] * z;
    out_dir[2] = t0[2] * x + t1[2] * y + normal[2] * z;
}

/// @brief Estimate incoming irradiance at a surface point (direct + bounced + sky).
/// @details Adds copied-light direct illumination, then recursively follows one
///   deterministic cosine-weighted bounce per level. Misses add sky color;
///   hits apply receiving-triangle albedo and emission.
/// @param baker Baker containing BVH, copied lights, and sky color.
/// @param point Three-element world-space shading point.
/// @param normal Three-element oriented unit normal.
/// @param rng In/out deterministic sampler state.
/// @param bounces Remaining nonnegative indirect-bounce depth.
/// @param out_rgb Output three-element irradiance.
static void baker_radiance(const rt_lightbaker3d *baker,
                           const double point[3],
                           const double normal[3],
                           uint32_t *rng,
                           int32_t bounces,
                           double out_rgb[3]) {
    baker_direct_light(baker, point, normal, out_rgb);
    if (bounces <= 0)
        return;
    double origin[3] = {point[0] + normal[0] * BAKER3D_EPS * 4,
                        point[1] + normal[1] * BAKER3D_EPS * 4,
                        point[2] + normal[2] * BAKER3D_EPS * 4};
    double dir[3];
    baker_cosine_dir(rng, normal, dir);
    double t;
    int32_t hit = baker_trace(baker, origin, dir, 1e12, &t);
    if (hit < 0) {
        out_rgb[0] += baker->sky_color[0];
        out_rgb[1] += baker->sky_color[1];
        out_rgb[2] += baker->sky_color[2];
        return;
    }
    const baker_tri *tri = &baker->tris[hit];
    double hp[3] = {origin[0] + dir[0] * t, origin[1] + dir[1] * t, origin[2] + dir[2] * t};
    double hn[3] = {tri->normal[0], tri->normal[1], tri->normal[2]};
    if (hn[0] * dir[0] + hn[1] * dir[1] + hn[2] * dir[2] > 0.0) {
        hn[0] = -hn[0];
        hn[1] = -hn[1];
        hn[2] = -hn[2];
    }
    double bounce_rgb[3];
    baker_radiance(baker, hp, hn, rng, bounces - 1, bounce_rgb);
    /* Cosine-weighted sampling folds the ndl/pdf terms; albedo modulates. */
    out_rgb[0] += (bounce_rgb[0] * tri->albedo[0] + tri->emissive[0]);
    out_rgb[1] += (bounce_rgb[1] * tri->albedo[1] + tri->emissive[1]);
    out_rgb[2] += (bounce_rgb[2] * tri->albedo[2] + tri->emissive[2]);
}

/*==========================================================================
 * LightBaker3D public surface
 *=========================================================================*/

/// @brief Finalize a baker and release its retained/native resources.
/// @param obj LightBaker3D payload; `NULL` is ignored.
static void lightbaker3d_finalize(void *obj) {
    rt_lightbaker3d *baker = (rt_lightbaker3d *)obj;
    if (!baker)
        return;
    if (baker->scene && rt_obj_release_check0(baker->scene))
        rt_obj_free(baker->scene);
    baker->scene = NULL;
    if (baker->atlas && rt_obj_release_check0(baker->atlas))
        rt_obj_free(baker->atlas);
    baker->atlas = NULL;
    free(baker->tris);
    free(baker->bvh_order);
    free(baker->bvh_nodes);
    free(baker->atlas_hdr);
    free(baker->atlas_coverage);
}

/// @brief Create a deterministic light baker for one retained Scene3D.
/// @param scene Live Scene3D retained until baker finalization.
/// @return New GC-managed baker with default density, samples, bounces, and
///   atlas size, or `NULL` after reporting invalid input or allocation failure.
void *rt_lightbaker3d_new(void *scene) {
    if (!scene || !rt_g3d_has_class(scene, RT_G3D_SCENE3D_CLASS_ID)) {
        rt_trap("LightBaker3D.New: scene must be a SceneGraph");
        return NULL;
    }
    rt_lightbaker3d *baker = (rt_lightbaker3d *)rt_obj_new_i64(RT_G3D_LIGHTBAKER3D_CLASS_ID,
                                                               (int64_t)sizeof(rt_lightbaker3d));
    if (!baker) {
        rt_trap("LightBaker3D.New: allocation failed");
        return NULL;
    }
    memset(baker, 0, sizeof(*baker));
    rt_obj_set_finalizer(baker, lightbaker3d_finalize);
    rt_obj_retain_maybe(scene);
    baker->scene = scene;
    baker->texels_per_unit = 8.0;
    baker->samples = 64;
    baker->bounces = 2;
    baker->atlas_dim = BAKER3D_ATLAS_DIM;
    return baker;
}

/// @brief Resolve a LightBaker3D and report a caller-specific trap on failure.
/// @param obj Candidate runtime object.
/// @param method Trap message used for invalid input.
/// @return Borrowed baker implementation, or `NULL`.
static rt_lightbaker3d *lightbaker3d_checked(void *obj, const char *method) {
    rt_lightbaker3d *baker =
        (rt_lightbaker3d *)rt_g3d_checked_or_null(obj, RT_G3D_LIGHTBAKER3D_CLASS_ID);
    if (!baker)
        rt_trap(method);
    return baker;
}

/// @brief Set the chart-density target for subsequent bake steps.
/// @param obj LightBaker3D receiver; invalid handles report a trap.
/// @param texels Positive texels per world unit, capped at 64; other values are ignored.
void rt_lightbaker3d_set_texels_per_unit(void *obj, double texels) {
    rt_lightbaker3d *baker =
        lightbaker3d_checked(obj, "LightBaker3D.set_TexelsPerUnit: invalid baker");
    if (baker && isfinite(texels) && texels > 0.0)
        baker->texels_per_unit = texels > 64.0 ? 64.0 : texels;
}

/// @brief Return the configured lightmap texel density.
/// @param obj LightBaker3D receiver.
/// @return Texels per world unit, or zero after invalid-handle reporting.
double rt_lightbaker3d_get_texels_per_unit(void *obj) {
    rt_lightbaker3d *baker =
        lightbaker3d_checked(obj, "LightBaker3D.get_TexelsPerUnit: invalid baker");
    return baker ? baker->texels_per_unit : 0.0;
}

/// @brief Set deterministic samples evaluated per lightmap texel.
/// @param obj LightBaker3D receiver.
/// @param samples Positive sample count capped at 1024; nonpositive values are ignored.
void rt_lightbaker3d_set_samples(void *obj, int64_t samples) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.set_Samples: invalid baker");
    if (baker && samples > 0)
        baker->samples = samples > 1024 ? 1024 : samples;
}

/// @brief Return the configured per-texel sample count.
/// @param obj LightBaker3D receiver.
/// @return Sample count, or zero after invalid-handle reporting.
int64_t rt_lightbaker3d_get_samples(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.get_Samples: invalid baker");
    return baker ? baker->samples : 0;
}

/// @brief Set maximum recursive indirect-light bounce depth.
/// @param obj LightBaker3D receiver.
/// @param bounces Nonnegative depth capped at eight; negative values are ignored.
void rt_lightbaker3d_set_bounces(void *obj, int64_t bounces) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.set_Bounces: invalid baker");
    if (baker && bounces >= 0)
        baker->bounces = bounces > 8 ? 8 : bounces;
}

/// @brief Return the configured indirect bounce depth.
/// @param obj LightBaker3D receiver.
/// @return Bounce count, or zero after invalid-handle reporting.
int64_t rt_lightbaker3d_get_bounces(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.get_Bounces: invalid baker");
    return baker ? baker->bounces : 0;
}

/// @brief Set nonnegative radiance returned by rays that miss the scene.
/// @param obj LightBaker3D receiver.
/// @param r Red sky radiance; nonfinite or nonpositive input becomes zero.
/// @param g Green sky radiance; nonfinite or nonpositive input becomes zero.
/// @param b Blue sky radiance; nonfinite or nonpositive input becomes zero.
void rt_lightbaker3d_set_sky_color(void *obj, double r, double g, double b) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.SetSkyColor: invalid baker");
    if (!baker)
        return;
    baker->sky_color[0] = isfinite(r) && r > 0.0 ? r : 0.0;
    baker->sky_color[1] = isfinite(g) && g > 0.0 ? g : 0.0;
    baker->sky_color[2] = isfinite(b) && b > 0.0 ? b : 0.0;
}

/// @brief Return normalized triangle-bake progress.
/// @param obj LightBaker3D receiver.
/// @return Stored progress in ordinary operation, or zero after invalid-handle reporting.
double rt_lightbaker3d_get_progress(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.get_Progress: invalid baker");
    return baker ? baker->progress : 0.0;
}

/// @brief Snapshot one enabled non-ambient Light3D into the bake input.
/// @details Copies type, transform, color, intensity, and attenuation immediately;
///   the baker does not retain the source light and later mutations do not affect
///   this bake. Inputs beyond the sixteen-light cap are ignored.
/// @param obj LightBaker3D receiver.
/// @param light_obj Light3D to copy; invalid handles report a trap.
void rt_lightbaker3d_add_light(void *obj, void *light_obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.AddLight: invalid baker");
    rt_light3d *light = (rt_light3d *)rt_g3d_checked_or_null(light_obj, RT_G3D_LIGHT3D_CLASS_ID);
    if (!baker || !light) {
        if (baker)
            rt_trap("LightBaker3D.AddLight: light must be a Light3D");
        return;
    }
    if (baker->light_count >= BAKER3D_MAX_LIGHTS || light->type == 2 || !light->enabled)
        return;
    baker_light *slot = &baker->lights[baker->light_count++];
    slot->type = light->type;
    memcpy(slot->direction, light->direction, sizeof(slot->direction));
    memcpy(slot->position, light->position, sizeof(slot->position));
    memcpy(slot->color, light->color, sizeof(slot->color));
    slot->intensity = light->intensity;
    slot->attenuation = light->attenuation;
}

/// @brief Allocate a chart rect on the atlas shelf packer. Returns 0 when full.
/// @param baker Baker containing atlas dimensions and mutable shelf cursor.
/// @param w Positive chart width in texels.
/// @param h Positive chart height in texels.
/// @param x Output atlas X origin.
/// @param y Output atlas Y origin.
/// @return Nonzero when the padded chart fits and cursor state advances; zero
///   when the atlas is full.
static int baker_alloc_chart(rt_lightbaker3d *baker, int32_t w, int32_t h, int32_t *x, int32_t *y) {
    if (baker->cursor_x + w + 1 > baker->atlas_dim) {
        baker->cursor_x = 0;
        baker->cursor_y += baker->row_height + 1;
        baker->row_height = 0;
    }
    if (baker->cursor_y + h + 1 > baker->atlas_dim)
        return 0;
    *x = baker->cursor_x;
    *y = baker->cursor_y;
    baker->cursor_x += w + 1;
    if (h > baker->row_height)
        baker->row_height = h;
    return 1;
}

/// @brief Run one bake slice: gathers on the first call, then bakes triangles
///   in deterministic order until the slice budget is spent.
/// @details Each call processes at most 64 charts, writes source mesh UV1 values,
///   and updates progress. Terminal gather/BVH/allocation failure is represented
///   as a completed bake without an atlas. Completion dilates one texel ring and
///   publishes an 8-bit atlas with two-times radiance headroom.
/// @param obj LightBaker3D receiver.
/// @return 1 when the bake is complete, 0 when more steps remain.
int8_t rt_lightbaker3d_bake_step(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.BakeStep: invalid baker");
    if (!baker)
        return 1;
    if (baker->done)
        return 1;
    if (!baker->gathered) {
        if (!baker_gather_scene(baker) || !baker_bvh_build(baker)) {
            baker->done = 1;
            baker->progress = 1.0;
            return 1;
        }
        baker->atlas_hdr =
            (float *)calloc((size_t)baker->atlas_dim * baker->atlas_dim * 3, sizeof(float));
        baker->atlas_coverage = (uint8_t *)calloc((size_t)baker->atlas_dim * baker->atlas_dim, 1);
        if (!baker->atlas_hdr || !baker->atlas_coverage) {
            baker->done = 1;
            baker->progress = 1.0;
            return 1;
        }
    }

    int32_t budget = 64; /* triangles per step: keeps editor slices responsive */
    while (baker->next_tri < baker->tri_count && budget-- > 0) {
        baker_tri *tri = &baker->tris[baker->next_tri];
        baker_node_entry *entry = &baker->nodes[tri->node_index];
        /* Chart size from world edge lengths x texel density. */
        double e1_len = sqrt(pow(tri->p1[0] - tri->p0[0], 2) + pow(tri->p1[1] - tri->p0[1], 2) +
                             pow(tri->p1[2] - tri->p0[2], 2));
        double e2_len = sqrt(pow(tri->p2[0] - tri->p0[0], 2) + pow(tri->p2[1] - tri->p0[1], 2) +
                             pow(tri->p2[2] - tri->p0[2], 2));
        int32_t w = (int32_t)ceil(e1_len * baker->texels_per_unit) + 2;
        int32_t h = (int32_t)ceil(e2_len * baker->texels_per_unit) + 2;
        if (w < 3)
            w = 3;
        if (h < 3)
            h = 3;
        if (w > 128)
            w = 128;
        if (h > 128)
            h = 128;
        int32_t cx, cy;
        if (!baker_alloc_chart(baker, w, h, &cx, &cy)) {
            baker->next_tri = baker->tri_count; /* atlas full: stop (documented cap) */
            break;
        }
        /* Bake texels: chart parameterizes the triangle by (u,v) barycentrics with
         * u along edge1 and v along edge2 (u + v <= 1 covers the triangle; the
         * spill texels clamp so dilation has valid neighbors). */
        for (int32_t ty = 0; ty < h; ++ty) {
            for (int32_t tx = 0; tx < w; ++tx) {
                double u = (double)tx / (double)(w - 1);
                double v = (double)ty / (double)(h - 1);
                if (u + v > 1.0) {
                    double excess = (u + v - 1.0) * 0.5;
                    u -= excess;
                    v -= excess;
                    if (u < 0.0)
                        u = 0.0;
                    if (v < 0.0)
                        v = 0.0;
                }
                double point[3] = {
                    tri->p0[0] + (tri->p1[0] - tri->p0[0]) * u + (tri->p2[0] - tri->p0[0]) * v,
                    tri->p0[1] + (tri->p1[1] - tri->p0[1]) * u + (tri->p2[1] - tri->p0[1]) * v,
                    tri->p0[2] + (tri->p1[2] - tri->p0[2]) * u + (tri->p2[2] - tri->p0[2]) * v};
                uint32_t rng = (uint32_t)(baker->next_tri * 9781 + ty * 6271 + tx * 26699 + 1);
                double acc[3] = {0.0, 0.0, 0.0};
                int64_t samples = baker->samples < 1 ? 1 : baker->samples;
                for (int64_t si = 0; si < samples; ++si) {
                    double rgb[3];
                    baker_radiance(baker, point, tri->normal, &rng, (int32_t)baker->bounces, rgb);
                    acc[0] += rgb[0];
                    acc[1] += rgb[1];
                    acc[2] += rgb[2];
                }
                size_t at = ((size_t)(cy + ty) * baker->atlas_dim + (cx + tx));
                baker->atlas_hdr[at * 3 + 0] = (float)(acc[0] / (double)samples);
                baker->atlas_hdr[at * 3 + 1] = (float)(acc[1] / (double)samples);
                baker->atlas_hdr[at * 3 + 2] = (float)(acc[2] / (double)samples);
                baker->atlas_coverage[at] = 1;
            }
        }
        /* Write TEXCOORD_1 chart coordinates into the source mesh triangle. */
        rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(entry->mesh, RT_G3D_MESH3D_CLASS_ID);
        if (mesh) {
            int64_t idx[3];
            if (rt_mesh3d_get_triangle_raw(entry->mesh, tri->tri_index, idx)) {
                double inv_dim = 1.0 / (double)baker->atlas_dim;
                double u0 = (cx + 0.5) * inv_dim;
                double v0 = (cy + 0.5) * inv_dim;
                double u1 = (cx + w - 1.5) * inv_dim;
                double v1 = (cy + h - 1.5) * inv_dim;
                for (int k = 0; k < 3; ++k) {
                    if (idx[k] < 0 || idx[k] >= (int64_t)mesh->vertex_count)
                        continue;
                    vgfx3d_vertex_t *vert = &mesh->vertices[idx[k]];
                    if (k == 0) {
                        vert->uv1[0] = (float)u0;
                        vert->uv1[1] = (float)v0;
                    } else if (k == 1) {
                        vert->uv1[0] = (float)u1;
                        vert->uv1[1] = (float)v0;
                    } else {
                        vert->uv1[0] = (float)u0;
                        vert->uv1[1] = (float)v1;
                    }
                }
                rt_mesh3d_touch_geometry(mesh);
            }
        }
        baker->next_tri++;
    }
    baker->progress =
        baker->tri_count > 0 ? (double)baker->next_tri / (double)baker->tri_count : 1.0;
    if (baker->next_tri < baker->tri_count)
        return 0;

    /* Finish: dilate uncovered texels from covered neighbors and publish the atlas. */
    void *atlas = rt_pixels_new(baker->atlas_dim, baker->atlas_dim);
    if (atlas) {
        for (int32_t y = 0; y < baker->atlas_dim; ++y) {
            for (int32_t x = 0; x < baker->atlas_dim; ++x) {
                size_t at = (size_t)y * baker->atlas_dim + x;
                float r = baker->atlas_hdr[at * 3 + 0];
                float g = baker->atlas_hdr[at * 3 + 1];
                float b = baker->atlas_hdr[at * 3 + 2];
                if (!baker->atlas_coverage[at]) {
                    /* Single-ring dilation. */
                    float best[3] = {0.0f, 0.0f, 0.0f};
                    int found = 0;
                    for (int dy = -1; dy <= 1 && !found; ++dy) {
                        for (int dx = -1; dx <= 1 && !found; ++dx) {
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= baker->atlas_dim ||
                                ny >= baker->atlas_dim)
                                continue;
                            size_t nat = (size_t)ny * baker->atlas_dim + nx;
                            if (baker->atlas_coverage[nat]) {
                                best[0] = baker->atlas_hdr[nat * 3 + 0];
                                best[1] = baker->atlas_hdr[nat * 3 + 1];
                                best[2] = baker->atlas_hdr[nat * 3 + 2];
                                found = 1;
                            }
                        }
                    }
                    r = best[0];
                    g = best[1];
                    b = best[2];
                }
                /* Store with 2x headroom: value 2.0 -> 255. */
                int64_t ir = (int64_t)(fminf(fmaxf(r * 0.5f, 0.0f), 1.0f) * 255.0f + 0.5f);
                int64_t ig = (int64_t)(fminf(fmaxf(g * 0.5f, 0.0f), 1.0f) * 255.0f + 0.5f);
                int64_t ib = (int64_t)(fminf(fmaxf(b * 0.5f, 0.0f), 1.0f) * 255.0f + 0.5f);
                rt_pixels_set(atlas, x, y, (ir << 24) | (ig << 16) | (ib << 8) | 0xFF);
            }
        }
        if (baker->atlas && rt_obj_release_check0(baker->atlas))
            rt_obj_free(baker->atlas);
        baker->atlas = atlas;
    }
    baker->done = 1;
    baker->progress = 1.0;
    return 1;
}

/// @brief Install the baked atlas on every baked node via material instances.
/// @details Clones each source material or creates a default, assigns the
///   retained atlas, installs the instance on its scene node, and releases the
///   temporary creation reference. Incomplete or atlas-less bakes are ignored.
/// @param obj LightBaker3D receiver.
void rt_lightbaker3d_apply(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.Apply: invalid baker");
    if (!baker || !baker->done || !baker->atlas)
        return;
    for (int32_t n = 0; n < baker->node_count; ++n) {
        baker_node_entry *entry = &baker->nodes[n];
        void *material = entry->material;
        void *instance = NULL;
        if (material) {
            instance = rt_material3d_make_instance(material);
        } else {
            instance = rt_material3d_new();
        }
        if (!instance)
            continue;
        rt_material3d_set_lightmap(instance, baker->atlas);
        rt_scene_node3d_set_material(entry->node, instance);
        if (rt_obj_release_check0(instance))
            rt_obj_free(instance);
    }
}

/// @brief Acquire the completed lightmap atlas.
/// @param obj LightBaker3D receiver.
/// @return Retained Pixels handle that the caller must eventually release, or
///   `NULL` when unavailable or the receiver is invalid.
void *rt_lightbaker3d_get_atlas(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.get_Atlas: invalid baker");
    if (!baker || !baker->atlas)
        return NULL;
    rt_obj_retain_maybe(baker->atlas);
    return baker->atlas;
}

/*==========================================================================
 * LightProbeGrid3D — SH-9 irradiance probes
 *=========================================================================*/

typedef struct rt_lightprobegrid3d {
    void *vptr;
    double min_b[3];
    double spacing;
    int32_t nx, ny, nz;
    float *sh; /* probe-major: [probe][9][rgb] */
    uint8_t *valid;
    int8_t baked;
} rt_lightprobegrid3d;

/// @brief Finalize a probe grid and free coefficient/validity arrays.
/// @param obj LightProbeGrid3D payload; `NULL` is ignored.
static void lightprobegrid3d_finalize(void *obj) {
    rt_lightprobegrid3d *grid = (rt_lightprobegrid3d *)obj;
    if (!grid)
        return;
    free(grid->sh);
    free(grid->valid);
}

/// @brief Create a bounded regular SH-9 irradiance probe grid.
/// @details Copies Vec3 bounds, defaults invalid or tiny spacing to one,
///   computes two through sixty-four samples per axis, and allocates zeroed
///   probe-major coefficient and validity arrays.
/// @param min_v Vec3 minimum grid origin, copied on success.
/// @param max_v Vec3 requested maximum extent, copied only through derived counts.
/// @param spacing Positive world-unit distance between probes.
/// @return New GC-managed grid, or `NULL` after reporting invalid bounds or
///   object allocation failure.
void *rt_lightprobegrid3d_new(void *min_v, void *max_v, double spacing) {
    double min_b[3], max_b[3];
    if (!baker_read_vec3(min_v, &min_b[0], &min_b[1], &min_b[2]) ||
        !baker_read_vec3(max_v, &max_b[0], &max_b[1], &max_b[2])) {
        rt_trap("LightProbeGrid3D.New: bounds must be Vec3");
        return NULL;
    }
    if (!isfinite(spacing) || spacing <= 0.01)
        spacing = 1.0;
    rt_lightprobegrid3d *grid = (rt_lightprobegrid3d *)rt_obj_new_i64(
        RT_G3D_LIGHTPROBEGRID3D_CLASS_ID, (int64_t)sizeof(rt_lightprobegrid3d));
    if (!grid) {
        rt_trap("LightProbeGrid3D.New: allocation failed");
        return NULL;
    }
    memset(grid, 0, sizeof(*grid));
    rt_obj_set_finalizer(grid, lightprobegrid3d_finalize);
    memcpy(grid->min_b, min_b, sizeof(grid->min_b));
    grid->spacing = spacing;
    for (int a = 0; a < 3; ++a) {
        double extent = max_b[a] - min_b[a];
        int32_t n = extent > 0.0 ? (int32_t)floor(extent / spacing) + 2 : 2;
        if (n < 2)
            n = 2;
        if (n > 64)
            n = 64;
        if (a == 0)
            grid->nx = n;
        else if (a == 1)
            grid->ny = n;
        else
            grid->nz = n;
    }
    size_t probes = (size_t)grid->nx * grid->ny * grid->nz;
    grid->sh = (float *)calloc(probes * 27, sizeof(float));
    grid->valid = (uint8_t *)calloc(probes, 1);
    return grid;
}

/// @brief Resolve a LightProbeGrid3D and report a caller-specific trap on failure.
/// @param obj Candidate runtime object.
/// @param method Trap message used for invalid input.
/// @return Borrowed grid implementation, or `NULL`.
static rt_lightprobegrid3d *lightprobegrid3d_checked(void *obj, const char *method) {
    rt_lightprobegrid3d *grid =
        (rt_lightprobegrid3d *)rt_g3d_checked_or_null(obj, RT_G3D_LIGHTPROBEGRID3D_CLASS_ID);
    if (!grid)
        rt_trap(method);
    return grid;
}

/// @brief Return the total regular-grid probe count.
/// @param obj LightProbeGrid3D receiver.
/// @return Product of three axis counts, or zero after invalid-handle reporting.
int64_t rt_lightprobegrid3d_get_probe_count(void *obj) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.get_ProbeCount: invalid grid");
    return grid ? (int64_t)grid->nx * grid->ny * grid->nz : 0;
}

/// @brief Evaluate the 9 SH basis functions for a direction.
/// @param d Three-element sample direction.
/// @param out Output array of nine real SH basis values.
static void probe_sh_basis(const double d[3], double out[9]) {
    out[0] = 0.282095;
    out[1] = 0.488603 * d[1];
    out[2] = 0.488603 * d[2];
    out[3] = 0.488603 * d[0];
    out[4] = 1.092548 * d[0] * d[1];
    out[5] = 1.092548 * d[1] * d[2];
    out[6] = 0.315392 * (3.0 * d[2] * d[2] - 1.0);
    out[7] = 1.092548 * d[0] * d[2];
    out[8] = 0.546274 * (d[0] * d[0] - d[1] * d[1]);
}

/// @brief Bake the probe grid against a baker's gathered scene + lights.
/// @details Reuses the baker's BVH and radiance estimator so probes and
///   lightmaps agree; the baker must be constructed over the same scene (its
///   gather runs here when the baker has not baked yet). Directions are sampled
///   uniformly on the sphere with deterministic seeds. Probes likely inside
///   geometry are marked invalid and opportunistically in-filled from an
///   adjacent valid probe.
/// @param obj LightProbeGrid3D receiver whose coefficient arrays are overwritten.
/// @param baker_obj LightBaker3D providing scene BVH, copied lights, sky, samples,
///   and bounce depth.
void rt_lightprobegrid3d_bake(void *obj, void *baker_obj) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Bake: invalid grid");
    rt_lightbaker3d *baker =
        lightbaker3d_checked(baker_obj, "LightProbeGrid3D.Bake: invalid baker");
    if (!grid || !baker)
        return;
    if (!baker->gathered) {
        if (!baker_gather_scene(baker) || !baker_bvh_build(baker))
            return;
    }
    size_t probes = (size_t)grid->nx * grid->ny * grid->nz;
    int64_t dir_samples = baker->samples < 8 ? 8 : (baker->samples > 256 ? 256 : baker->samples);
    for (size_t p = 0; p < probes; ++p) {
        int32_t px = (int32_t)(p % grid->nx);
        int32_t py = (int32_t)((p / grid->nx) % grid->ny);
        int32_t pz = (int32_t)(p / ((size_t)grid->nx * grid->ny));
        double point[3] = {grid->min_b[0] + px * grid->spacing,
                           grid->min_b[1] + py * grid->spacing,
                           grid->min_b[2] + pz * grid->spacing};
        uint32_t rng = (uint32_t)(p * 40507 + 7);
        float *sh = grid->sh + p * 27;
        memset(sh, 0, 27 * sizeof(float));
        int inside_hits = 0;
        for (int64_t s = 0; s < dir_samples; ++s) {
            /* Uniform sphere direction. */
            double z = 1.0 - 2.0 * baker_lcg_unit(&rng);
            double phi = 2.0 * 3.14159265358979323846 * baker_lcg_unit(&rng);
            double rxy = sqrt(fmax(0.0, 1.0 - z * z));
            double dir[3] = {rxy * cos(phi), z, rxy * sin(phi)};
            double t;
            int32_t hit = baker_trace(baker, point, dir, 1e12, &t);
            double rgb[3];
            if (hit < 0) {
                rgb[0] = baker->sky_color[0];
                rgb[1] = baker->sky_color[1];
                rgb[2] = baker->sky_color[2];
            } else {
                const baker_tri *tri = &baker->tris[hit];
                double facing =
                    tri->normal[0] * dir[0] + tri->normal[1] * dir[1] + tri->normal[2] * dir[2];
                if (facing > 0.0 && t < grid->spacing * 0.5)
                    inside_hits++; /* backface very close: probably inside geometry */
                double hp[3] = {
                    point[0] + dir[0] * t, point[1] + dir[1] * t, point[2] + dir[2] * t};
                double hn[3] = {tri->normal[0], tri->normal[1], tri->normal[2]};
                if (facing > 0.0) {
                    hn[0] = -hn[0];
                    hn[1] = -hn[1];
                    hn[2] = -hn[2];
                }
                baker_radiance(baker, hp, hn, &rng, (int32_t)baker->bounces, rgb);
                rgb[0] = rgb[0] * tri->albedo[0] + tri->emissive[0];
                rgb[1] = rgb[1] * tri->albedo[1] + tri->emissive[1];
                rgb[2] = rgb[2] * tri->albedo[2] + tri->emissive[2];
            }
            double basis[9];
            probe_sh_basis(dir, basis);
            double weight = 4.0 * 3.14159265358979323846 / (double)dir_samples;
            for (int c = 0; c < 9; ++c) {
                sh[c * 3 + 0] += (float)(rgb[0] * basis[c] * weight);
                sh[c * 3 + 1] += (float)(rgb[1] * basis[c] * weight);
                sh[c * 3 + 2] += (float)(rgb[2] * basis[c] * weight);
            }
        }
        grid->valid[p] = inside_hits * 4 < dir_samples ? 1 : 0;
    }
    /* In-fill invalid probes from the nearest valid axis neighbor. */
    for (size_t p = 0; p < probes; ++p) {
        if (grid->valid[p])
            continue;
        static const int32_t offs[6][3] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        int32_t px = (int32_t)(p % grid->nx);
        int32_t py = (int32_t)((p / grid->nx) % grid->ny);
        int32_t pz = (int32_t)(p / ((size_t)grid->nx * grid->ny));
        for (int o = 0; o < 6; ++o) {
            int32_t qx = px + offs[o][0];
            int32_t qy = py + offs[o][1];
            int32_t qz = pz + offs[o][2];
            if (qx < 0 || qy < 0 || qz < 0 || qx >= grid->nx || qy >= grid->ny || qz >= grid->nz)
                continue;
            size_t q = ((size_t)qz * grid->ny + qy) * grid->nx + qx;
            if (grid->valid[q]) {
                memcpy(grid->sh + p * 27, grid->sh + q * 27, 27 * sizeof(float));
                break;
            }
        }
    }
    grid->baked = 1;
}

/// @brief Trilinear-sample the grid's SH irradiance for @p normal at @p position.
/// @details Clamps the containing cell to the grid boundary, blends all eight
///   probe coefficient sets, applies cosine-convolution factors, and clamps
///   negative or nonfinite output. A missing normal uses positive Y.
/// @param obj Baked or loaded LightProbeGrid3D receiver.
/// @param position Vec3 world-space sample position; invalid input returns black.
/// @param normal Optional Vec3 surface normal.
/// @return Newly allocated Vec3 irradiance, or a newly allocated black Vec3
///   for invalid grid/position input.
void *rt_lightprobegrid3d_sample(void *obj, void *position, void *normal) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Sample: invalid grid");
    double pos[3], nrm[3] = {0.0, 1.0, 0.0};
    if (!grid || !baker_read_vec3(position, &pos[0], &pos[1], &pos[2]))
        return rt_vec3_new(0.0, 0.0, 0.0);
    if (normal)
        (void)baker_read_vec3(normal, &nrm[0], &nrm[1], &nrm[2]);
    double nlen = sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    if (nlen > 1e-9) {
        nrm[0] /= nlen;
        nrm[1] /= nlen;
        nrm[2] /= nlen;
    }
    double gx = (pos[0] - grid->min_b[0]) / grid->spacing;
    double gy = (pos[1] - grid->min_b[1]) / grid->spacing;
    double gz = (pos[2] - grid->min_b[2]) / grid->spacing;
    int32_t x0 = (int32_t)floor(gx), y0 = (int32_t)floor(gy), z0 = (int32_t)floor(gz);
    double fx = gx - x0, fy = gy - y0, fz = gz - z0;
    if (x0 < 0) {
        x0 = 0;
        fx = 0.0;
    }
    if (y0 < 0) {
        y0 = 0;
        fy = 0.0;
    }
    if (z0 < 0) {
        z0 = 0;
        fz = 0.0;
    }
    if (x0 >= grid->nx - 1) {
        x0 = grid->nx - 2;
        fx = 1.0;
    }
    if (y0 >= grid->ny - 1) {
        y0 = grid->ny - 2;
        fy = 1.0;
    }
    if (z0 >= grid->nz - 1) {
        z0 = grid->nz - 2;
        fz = 1.0;
    }
    double sh[27];
    memset(sh, 0, sizeof(sh));
    for (int corner = 0; corner < 8; ++corner) {
        int32_t cx = x0 + (corner & 1);
        int32_t cy = y0 + ((corner >> 1) & 1);
        int32_t cz = z0 + ((corner >> 2) & 1);
        double w = ((corner & 1) ? fx : 1.0 - fx) * (((corner >> 1) & 1) ? fy : 1.0 - fy) *
                   (((corner >> 2) & 1) ? fz : 1.0 - fz);
        const float *psh = grid->sh + (((size_t)cz * grid->ny + cy) * grid->nx + cx) * 27;
        for (int c = 0; c < 27; ++c)
            sh[c] += psh[c] * w;
    }
    /* Cosine-convolved irradiance evaluation (Ramamoorthi-Hanrahan constants). */
    double basis[9];
    probe_sh_basis(nrm, basis);
    static const double conv[9] = {
        3.141593, 2.094395, 2.094395, 2.094395, 0.785398, 0.785398, 0.785398, 0.785398, 0.785398};
    double out[3] = {0.0, 0.0, 0.0};
    for (int c = 0; c < 9; ++c) {
        double k = basis[c] * conv[c] / 3.141593;
        out[0] += sh[c * 3 + 0] * k;
        out[1] += sh[c * 3 + 1] * k;
        out[2] += sh[c * 3 + 2] * k;
    }
    for (int a = 0; a < 3; ++a)
        if (!isfinite(out[a]) || out[a] < 0.0)
            out[a] = 0.0;
    return rt_vec3_new(out[0], out[1], out[2]);
}

/*==========================================================================
 * Serialization (.vlpg — versioned native-binary probe grids)
 *=========================================================================*/

#define VLPG_MAGIC "VLPG0001"

/// @brief Save a baked probe grid to the versioned VLPG binary layout.
/// @details Writes magic, native double/int metadata, validity bytes, and
///   probe-major float coefficients. The file is not published transactionally.
/// @param obj Baked LightProbeGrid3D receiver.
/// @param path Runtime string naming the output filesystem path.
/// @return Nonzero only when every write and file open succeeds.
int8_t rt_lightprobegrid3d_save(void *obj, rt_string path) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Save: invalid grid");
    const char *cpath = path ? rt_string_cstr(path) : NULL;
    if (!grid || !cpath || !grid->baked)
        return 0;
    FILE *f = fopen(cpath, "wb");
    if (!f)
        return 0;
    size_t probes = (size_t)grid->nx * grid->ny * grid->nz;
    int ok = fwrite(VLPG_MAGIC, 1, 8, f) == 8 && fwrite(grid->min_b, sizeof(double), 3, f) == 3 &&
             fwrite(&grid->spacing, sizeof(double), 1, f) == 1 &&
             fwrite(&grid->nx, sizeof(int32_t), 1, f) == 1 &&
             fwrite(&grid->ny, sizeof(int32_t), 1, f) == 1 &&
             fwrite(&grid->nz, sizeof(int32_t), 1, f) == 1 &&
             fwrite(grid->valid, 1, probes, f) == probes &&
             fwrite(grid->sh, sizeof(float), probes * 27, f) == probes * 27;
    fclose(f);
    return ok ? 1 : 0;
}

/// @brief Load a versioned VLPG binary into an existing probe grid.
/// @details Validates magic, dimensions, and spacing, then reads replacement
///   arrays before freeing current storage so malformed or truncated files
///   leave the grid unchanged.
/// @param obj LightProbeGrid3D receiver to replace on success.
/// @param path Runtime string naming the input filesystem path.
/// @return Nonzero on a complete valid load; zero for invalid input, I/O,
///   validation, or allocation failure.
int8_t rt_lightprobegrid3d_load(void *obj, rt_string path) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Load: invalid grid");
    const char *cpath = path ? rt_string_cstr(path) : NULL;
    if (!grid || !cpath)
        return 0;
    FILE *f = fopen(cpath, "rb");
    if (!f)
        return 0;
    char magic[8] = {0};
    double min_b[3] = {0.0, 0.0, 0.0};
    double spacing = 0.0;
    int32_t nx = 0;
    int32_t ny = 0;
    int32_t nz = 0;
    int ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, VLPG_MAGIC, 8) == 0 &&
             fread(min_b, sizeof(double), 3, f) == 3 &&
             fread(&spacing, sizeof(double), 1, f) == 1 && fread(&nx, sizeof(int32_t), 1, f) == 1 &&
             fread(&ny, sizeof(int32_t), 1, f) == 1 && fread(&nz, sizeof(int32_t), 1, f) == 1;
    if (ok && (nx < 2 || ny < 2 || nz < 2 || nx > 64 || ny > 64 || nz > 64 || !isfinite(spacing) ||
               spacing <= 0.0))
        ok = 0;
    if (ok) {
        size_t probes = (size_t)nx * ny * nz;
        uint8_t *valid = (uint8_t *)malloc(probes);
        float *sh = (float *)malloc(probes * 27 * sizeof(float));
        if (!valid || !sh || fread(valid, 1, probes, f) != probes ||
            fread(sh, sizeof(float), probes * 27, f) != probes * 27) {
            free(valid);
            free(sh);
            ok = 0;
        } else {
            free(grid->valid);
            free(grid->sh);
            grid->valid = valid;
            grid->sh = sh;
            memcpy(grid->min_b, min_b, sizeof(grid->min_b));
            grid->spacing = spacing;
            grid->nx = nx;
            grid->ny = ny;
            grid->nz = nz;
            grid->baked = 1;
        }
    }
    fclose(f);
    return ok ? 1 : 0;
}

#else
typedef int rt_lightbaker3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
