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
//   seam-safe charts) and LightProbeGrid3D (SH-9 irradiance probe grid for
//   dynamic objects), with .vlm/.vlpg serialization.
// Key invariants:
//   - Bakes are deterministic: fixed per-texel/per-probe sample seeds, no
//     wall-clock or thread-order dependence.
//   - Charts publish through per-node mesh copies with unique seam vertices;
//     the atlas applies through Material3D lightmap slots on material instances.
// Ownership/Lifetime:
//   - Baker/grid are GC-managed; the baker retains its scene and output atlas.
//   - Explicit light state is copied at AddLight time rather than retained.
//   - Bakers and grids own their native BVH, atlas, validity, and SH arrays.
// Links: docs/adr/0088, rt_lightbaker3d.h.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic lightmap baking and SH-9 irradiance probe grids.
/// @details Static scene geometry is flattened into a triangle BVH, sampled by
///   deterministic direct/indirect radiance estimators, packed into a lightmap
///   atlas, and reused to bake, sample, save, and load spatial probe grids.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_lightbaker3d.h"
#include "rt_alloc_size.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_morphtarget3d.h"
#include "rt_parallel.h"
#include "rt_pixels_internal.h"
#include "rt_scene3d.h"
#include "rt_threadpool.h"
#include "rt_trap.h"
#include "rt_vec3.h"

#include <float.h>
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
    double values[3];
    if (!x || !y || !z || !rt_g3d_is_vec3(v))
        return 0;
    values[0] = rt_vec3_x(v);
    values[1] = rt_vec3_y(v);
    values[2] = rt_vec3_z(v);
    if (!isfinite(values[0]) || !isfinite(values[1]) || !isfinite(values[2]))
        return 0;
    *x = values[0];
    *y = values[1];
    *z = values[2];
    return 1;
}

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int32_t rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
extern void *rt_pixels_new(int64_t width, int64_t height);

#define BAKER3D_MAX_TRIS 262144
#define BAKER3D_MAX_NODES 4096
#define BAKER3D_MAX_TRAVERSAL_NODES 65536
#define BAKER3D_ATLAS_DIM 1024
#define BAKER3D_MAX_CHART_DIM 128
#define BAKER3D_EPS 1e-4
#define BAKER3D_SAMPLE_WORK_PER_STEP 1024
#define BAKER3D_MAX_PARALLEL_TASKS 32

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
    int32_t node_index;         /* source baked-node index */
    int32_t tri_index;          /* triangle index within the source mesh */
    uint32_t vertex_indices[3]; /* captured UV publication targets */
    int32_t chart_x, chart_y;
    int32_t chart_w, chart_h;
    int32_t chart_root;
    int32_t texel_min_x, texel_min_y;
    int32_t texel_max_x, texel_max_y;
    double chart_uv[3][2];
} baker_tri;

typedef struct baker_bvh_node {
    double min_b[3], max_b[3];
    int32_t left;  /* child index, or -1 for leaf */
    int32_t right; /* child index, or -1 for leaf */
    int32_t first; /* leaf: first tri index in order array */
    int32_t count; /* leaf: tri count */
} baker_bvh_node;

typedef struct baker_node_entry {
    void *node;     /* retained scene node */
    void *mesh;     /* retained mesh */
    void *material; /* retained material (may be NULL) */
    double world_matrix[16];
    double albedo[3];
    double emissive[3];
    int32_t first_tri;
    int32_t tri_count;
    uint32_t geometry_revision;
    int8_t uv1_written;
} baker_node_entry;

typedef struct baker_light {
    int32_t type; /* 0 directional, 1 point, 3 spot, 4 rectangle, 5 sphere, 6 volume */
    double direction[3];
    double position[3];
    double basis_u[3];
    double basis_v[3];
    double color[3];
    double intensity;
    double attenuation;
    double inner_cos;
    double outer_cos;
    double range;
    double width;
    double height;
    double radius;
    int32_t decay_type;
    int8_t casts_shadows;
} baker_light;

typedef struct baker_light_bvh_node {
    double min_b[3], max_b[3];
    int32_t left, right;
    int32_t first, count;
} baker_light_bvh_node;

typedef struct rt_lightbaker3d {
    void *vptr;
    void *scene; /* retained */
    double texels_per_unit;
    int64_t samples;
    int64_t bounces;
    int8_t include_direct;
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
    baker_light *lights;
    int32_t light_count;
    int32_t light_capacity;
    int32_t *bounded_light_order;
    int32_t bounded_light_count;
    int32_t *global_light_indices;
    int32_t global_light_count;
    baker_light_bvh_node *light_bvh_nodes;
    int32_t light_bvh_node_count;
    int8_t light_index_built;
    /* atlas + charts */
    void *atlas;             /* retained Pixels */
    float *atlas_hdr;        /* rgb accumulation at atlas resolution */
    uint8_t *atlas_coverage; /* 1 where a chart texel wrote */
    int32_t atlas_dim;
    int32_t next_tri;    /* BakeStep cursor over gathered triangles */
    int32_t next_texel;  /* linear chart texel cursor */
    int64_t next_sample; /* sample cursor within the current texel */
    double sample_acc[3];
    uint64_t completed_sample_work;
    uint64_t total_sample_work;
    void *worker_pool; /* retained baker-owned Threadpool, lazily created */
    int32_t worker_count;
    int8_t worker_pool_failed;
    int8_t gathered;
    int8_t failed;
    int8_t applied;
} rt_lightbaker3d;

/*==========================================================================
 * Gathering static geometry
 *=========================================================================*/

/// @brief Snapshot bake-relevant albedo and emissive color from a material.
/// @details Defaults to white diffuse and black emission, then copies finite,
///   sanitized fields directly from a validated Material3D implementation.
/// @param material Borrowed optional Material3D handle.
/// @param albedo Output three-element linear diffuse color.
/// @param emissive Output three-element intensity-scaled emission color.
static void baker_read_material_colors(void *material, double albedo[3], double emissive[3]) {
    rt_material3d *mat;
    albedo[0] = albedo[1] = albedo[2] = 1.0;
    emissive[0] = emissive[1] = emissive[2] = 0.0;
    mat = (rt_material3d *)rt_g3d_checked_or_null(material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (!mat)
        return;
    for (int axis = 0; axis < 3; ++axis) {
        double diffuse = mat->diffuse[axis];
        double emission = mat->emissive[axis] * mat->emissive_intensity;
        if (!isfinite(diffuse) || diffuse < 0.0)
            diffuse = 0.0;
        else if (diffuse > 1.0)
            diffuse = 1.0;
        if (!isfinite(emission) || emission < 0.0)
            emission = 0.0;
        albedo[axis] = diffuse;
        emissive[axis] = emission;
    }
}

/// @brief Release one optional retained runtime reference and clear its slot.
static void baker_release_ref(void **slot) {
    void *value;
    if (!slot || !*slot)
        return;
    value = *slot;
    *slot = NULL;
    if (rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Release every retained reference in a staged or committed node table.
static void baker_release_node_entries(baker_node_entry *nodes, int32_t node_count) {
    if (!nodes || node_count <= 0)
        return;
    if (node_count > BAKER3D_MAX_NODES)
        node_count = BAKER3D_MAX_NODES;
    for (int32_t i = 0; i < node_count; ++i) {
        baker_release_ref(&nodes[i].material);
        baker_release_ref(&nodes[i].mesh);
        baker_release_ref(&nodes[i].node);
    }
}

/// @brief Transform an object-space normal by a finite inverse-transpose 3x3.
/// @return Nonzero when a finite normalized world normal was produced.
static int baker_transform_normal(const double matrix[16], const double local[3], double world[3]) {
    double linear[9];
    double linear_scale = 0.0;
    double cofactors[9];
    double det;
    double length;
    if (!matrix || !local || !world)
        return 0;
    linear[0] = matrix[0];
    linear[1] = matrix[1];
    linear[2] = matrix[2];
    linear[3] = matrix[4];
    linear[4] = matrix[5];
    linear[5] = matrix[6];
    linear[6] = matrix[8];
    linear[7] = matrix[9];
    linear[8] = matrix[10];
    for (int component = 0; component < 9; ++component) {
        double magnitude = fabs(linear[component]);
        if (!isfinite(magnitude))
            return 0;
        if (magnitude > linear_scale)
            linear_scale = magnitude;
    }
    if (linear_scale == 0.0)
        return 0;
    for (int component = 0; component < 9; ++component)
        linear[component] /= linear_scale;
    cofactors[0] = linear[4] * linear[8] - linear[5] * linear[7];
    cofactors[1] = linear[5] * linear[6] - linear[3] * linear[8];
    cofactors[2] = linear[3] * linear[7] - linear[4] * linear[6];
    cofactors[3] = linear[2] * linear[7] - linear[1] * linear[8];
    cofactors[4] = linear[0] * linear[8] - linear[2] * linear[6];
    cofactors[5] = linear[1] * linear[6] - linear[0] * linear[7];
    cofactors[6] = linear[1] * linear[5] - linear[2] * linear[4];
    cofactors[7] = linear[2] * linear[3] - linear[0] * linear[5];
    cofactors[8] = linear[0] * linear[4] - linear[1] * linear[3];
    det = linear[0] * cofactors[0] + linear[1] * cofactors[1] + linear[2] * cofactors[2];
    if (!isfinite(det) || det == 0.0)
        return 0;
    world[0] = cofactors[0] * local[0] + cofactors[1] * local[1] + cofactors[2] * local[2];
    world[1] = cofactors[3] * local[0] + cofactors[4] * local[1] + cofactors[5] * local[2];
    world[2] = cofactors[6] * local[0] + cofactors[7] * local[1] + cofactors[8] * local[2];
    length = hypot(hypot(world[0], world[1]), world[2]);
    if (!isfinite(length) || length <= 1e-12)
        return 0;
    length = det < 0.0 ? -length : length;
    world[0] /= length;
    world[1] /= length;
    world[2] /= length;
    return 1;
}

/// @brief Grow the scene traversal stack without exceeding its hard node budget.
static int baker_grow_traversal_stack(void ***stack, int32_t *capacity, int32_t needed) {
    int32_t next_capacity;
    void **grown;
    if (!stack || !capacity || needed < 0 || needed > BAKER3D_MAX_TRAVERSAL_NODES)
        return 0;
    if (needed <= *capacity)
        return 1;
    next_capacity = *capacity > 0 ? *capacity : 64;
    while (next_capacity < needed) {
        if (next_capacity > BAKER3D_MAX_TRAVERSAL_NODES / 2) {
            next_capacity = BAKER3D_MAX_TRAVERSAL_NODES;
            break;
        }
        next_capacity *= 2;
    }
    grown = (void **)realloc(*stack, (size_t)next_capacity * sizeof(*grown));
    if (!grown)
        return 0;
    *stack = grown;
    *capacity = next_capacity;
    return 1;
}

/// @brief Flatten static scene meshes into deterministic world-space triangle soup.
/// @details Traverses a dynamically grown, hard-bounded explicit stack, copies
///   finite transformed positions and material colors, excludes malformed or
///   degenerate triangles, and preserves source vertex indices for chart UVs.
///   The complete staged snapshot commits atomically and is then reused.
/// @param baker Baker retaining the source scene and owning gathered arrays.
/// @return Nonzero when a complete snapshot (including an empty scene) is
///   available; zero for a missing root, allocation failure, or hard-cap overflow.
static int baker_gather_scene(rt_lightbaker3d *baker) {
    baker_tri *tris = NULL;
    baker_node_entry *nodes = NULL;
    void **stack = NULL;
    int32_t tri_count = 0;
    int32_t tri_capacity = 4096;
    int32_t node_count = 0;
    int32_t stack_count = 0;
    int32_t stack_capacity = 0;
    int32_t visited_count = 0;
    void *root;
    if (baker->gathered)
        return 1;
    root = rt_scene3d_get_root(baker->scene);
    if (!root)
        return 0;
    nodes = (baker_node_entry *)calloc(BAKER3D_MAX_NODES, sizeof(*nodes));
    tris = (baker_tri *)malloc((size_t)tri_capacity * sizeof(*tris));
    if (!nodes || !tris || !baker_grow_traversal_stack(&stack, &stack_capacity, 1))
        goto fail;
    stack[stack_count++] = root;
    while (stack_count > 0) {
        void *node = stack[--stack_count];
        rt_mesh3d *mesh_impl;
        int64_t tri_total;
        double matrix[16];
        void *material;
        double albedo[3];
        double emissive[3];
        baker_node_entry *entry;
        int32_t entry_first_tri;
        if (++visited_count > BAKER3D_MAX_TRAVERSAL_NODES)
            goto fail;
        int64_t child_count = rt_scene_node3d_child_count(node);
        for (int64_t i = 0; i < child_count; ++i) {
            void *child = rt_scene_node3d_get_child(node, i);
            if (child) {
                if (!baker_grow_traversal_stack(&stack, &stack_capacity, stack_count + 1))
                    goto fail;
                stack[stack_count++] = child;
            }
        }
        if (!rt_scene_node3d_get_static(node))
            continue;
        void *mesh = rt_scene_node3d_get_mesh(node);
        mesh_impl = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
        tri_total = mesh_impl ? rt_mesh3d_get_triangle_count(mesh) : 0;
        if (!mesh_impl || tri_total <= 0)
            continue;
        if (node_count >= BAKER3D_MAX_NODES)
            goto fail;
        if (!rt_scene_node3d_get_world_matrix_components(node, matrix))
            continue;
        for (int component = 0; component < 16; ++component)
            if (!isfinite(matrix[component]))
                goto skip_node;
        material = rt_scene_node3d_get_material(node);
        baker_read_material_colors(material, albedo, emissive);
        entry = &nodes[node_count];
        entry_first_tri = tri_count;
        entry->first_tri = entry_first_tri;
        for (int64_t t = 0; t < tri_total; ++t) {
            int64_t idx[3];
            double corners[3][3];
            double authored_local_normal[3] = {0.0, 0.0, 0.0};
            double authored_world_normal[3];
            double edge1[3];
            double edge2[3];
            double normal_length;
            baker_tri candidate = {0};
            int valid = 1;
            if (tri_count >= BAKER3D_MAX_TRIS)
                goto fail;
            if (!rt_mesh3d_get_triangle_raw(mesh, t, idx))
                continue;
            for (int k = 0; k < 3; ++k) {
                double pos[3];
                double vn[3];
                if (!rt_mesh3d_get_vertex_raw(mesh, idx[k], pos, vn, NULL)) {
                    valid = 0;
                    break;
                }
                authored_local_normal[0] += vn[0];
                authored_local_normal[1] += vn[1];
                authored_local_normal[2] += vn[2];
                corners[k][0] =
                    matrix[0] * pos[0] + matrix[1] * pos[1] + matrix[2] * pos[2] + matrix[3];
                corners[k][1] =
                    matrix[4] * pos[0] + matrix[5] * pos[1] + matrix[6] * pos[2] + matrix[7];
                corners[k][2] =
                    matrix[8] * pos[0] + matrix[9] * pos[1] + matrix[10] * pos[2] + matrix[11];
                if (!isfinite(corners[k][0]) || !isfinite(corners[k][1]) ||
                    !isfinite(corners[k][2])) {
                    valid = 0;
                    break;
                }
            }
            if (!valid)
                continue;
            edge1[0] = corners[1][0] - corners[0][0];
            edge1[1] = corners[1][1] - corners[0][1];
            edge1[2] = corners[1][2] - corners[0][2];
            edge2[0] = corners[2][0] - corners[0][0];
            edge2[1] = corners[2][1] - corners[0][1];
            edge2[2] = corners[2][2] - corners[0][2];
            candidate.normal[0] = edge1[1] * edge2[2] - edge1[2] * edge2[1];
            candidate.normal[1] = edge1[2] * edge2[0] - edge1[0] * edge2[2];
            candidate.normal[2] = edge1[0] * edge2[1] - edge1[1] * edge2[0];
            normal_length =
                hypot(hypot(candidate.normal[0], candidate.normal[1]), candidate.normal[2]);
            if (!isfinite(normal_length) || normal_length <= 1e-12)
                continue;
            candidate.normal[0] /= normal_length;
            candidate.normal[1] /= normal_length;
            candidate.normal[2] /= normal_length;
            if (baker_transform_normal(matrix, authored_local_normal, authored_world_normal) &&
                candidate.normal[0] * authored_world_normal[0] +
                        candidate.normal[1] * authored_world_normal[1] +
                        candidate.normal[2] * authored_world_normal[2] <
                    0.0) {
                candidate.normal[0] = -candidate.normal[0];
                candidate.normal[1] = -candidate.normal[1];
                candidate.normal[2] = -candidate.normal[2];
            }
            if (tri_count >= tri_capacity) {
                int32_t next_capacity =
                    tri_capacity <= BAKER3D_MAX_TRIS / 2 ? tri_capacity * 2 : BAKER3D_MAX_TRIS;
                baker_tri *grown =
                    (baker_tri *)realloc(tris, (size_t)next_capacity * sizeof(*grown));
                if (!grown)
                    goto fail;
                tris = grown;
                tri_capacity = next_capacity;
            }
            memcpy(candidate.p0, corners[0], sizeof(candidate.p0));
            memcpy(candidate.p1, corners[1], sizeof(candidate.p1));
            memcpy(candidate.p2, corners[2], sizeof(candidate.p2));
            memcpy(candidate.albedo, albedo, sizeof(candidate.albedo));
            memcpy(candidate.emissive, emissive, sizeof(candidate.emissive));
            candidate.node_index = node_count;
            candidate.tri_index = (int32_t)t;
            candidate.vertex_indices[0] = (uint32_t)idx[0];
            candidate.vertex_indices[1] = (uint32_t)idx[1];
            candidate.vertex_indices[2] = (uint32_t)idx[2];
            tris[tri_count++] = candidate;
            entry->tri_count++;
        }
        if (entry->tri_count > 0) {
            entry->node = node;
            entry->mesh = mesh;
            entry->material = material;
            memcpy(entry->world_matrix, matrix, sizeof(entry->world_matrix));
            memcpy(entry->albedo, albedo, sizeof(entry->albedo));
            memcpy(entry->emissive, emissive, sizeof(entry->emissive));
            entry->geometry_revision = mesh_impl->geometry_revision;
            rt_obj_retain_maybe(entry->node);
            rt_obj_retain_maybe(entry->mesh);
            rt_obj_retain_maybe(entry->material);
            node_count++;
        } else {
            tri_count = entry_first_tri;
            memset(entry, 0, sizeof(*entry));
        }
    skip_node:
        (void)0;
    }
    free(stack);
    if (tri_count == 0) {
        free(tris);
        tris = NULL;
    }
    baker_release_node_entries(baker->nodes, baker->node_count);
    memset(baker->nodes, 0, sizeof(baker->nodes));
    memcpy(baker->nodes, nodes, (size_t)node_count * sizeof(*nodes));
    free(nodes);
    free(baker->tris);
    free(baker->bvh_order);
    free(baker->bvh_nodes);
    baker->tris = tris;
    baker->tri_count = tri_count;
    baker->bvh_order = NULL;
    baker->bvh_nodes = NULL;
    baker->bvh_node_count = 0;
    baker->node_count = node_count;
    baker->gathered = 1;
    return 1;

fail:
    free(stack);
    free(tris);
    baker_release_node_entries(nodes, node_count + (node_count < BAKER3D_MAX_NODES ? 1 : 0));
    free(nodes);
    return 0;
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

/// @brief Compare two gathered triangles by one centroid axis and source index.
static int baker_centroid_compare(const rt_lightbaker3d *baker,
                                  int32_t lhs,
                                  int32_t rhs,
                                  int axis) {
    const baker_tri *a = &baker->tris[lhs];
    const baker_tri *b = &baker->tris[rhs];
    double ac = a->p0[axis] + a->p1[axis] + a->p2[axis];
    double bc = b->p0[axis] + b->p1[axis] + b->p2[axis];
    if (ac < bc)
        return -1;
    if (ac > bc)
        return 1;
    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

/// @brief Swap two entries in a BVH triangle-order array.
static void baker_order_swap(int32_t *lhs, int32_t *rhs) {
    int32_t tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
}

/// @brief Restore a max heap rooted at @p root within one order-array range.
static void baker_heap_sift_down(
    const rt_lightbaker3d *baker, int32_t first, int32_t count, int32_t root, int axis) {
    for (;;) {
        int32_t child = root * 2 + 1;
        int32_t largest = root;
        if (child < count && baker_centroid_compare(baker,
                                                    baker->bvh_order[first + child],
                                                    baker->bvh_order[first + largest],
                                                    axis) > 0)
            largest = child;
        if (child + 1 < count && baker_centroid_compare(baker,
                                                        baker->bvh_order[first + child + 1],
                                                        baker->bvh_order[first + largest],
                                                        axis) > 0)
            largest = child + 1;
        if (largest == root)
            return;
        baker_order_swap(&baker->bvh_order[first + root], &baker->bvh_order[first + largest]);
        root = largest;
    }
}

/// @brief Deterministically heapsort one BVH order range as introselect fallback.
static void baker_heap_sort(rt_lightbaker3d *baker, int32_t first, int32_t count, int axis) {
    for (int32_t root = count / 2; root > 0; --root)
        baker_heap_sift_down(baker, first, count, root - 1, axis);
    for (int32_t end = count - 1; end > 0; --end) {
        baker_order_swap(&baker->bvh_order[first], &baker->bvh_order[first + end]);
        baker_heap_sift_down(baker, first, end, 0, axis);
    }
}

/// @brief Place the requested centroid rank using deterministic introselect.
static void baker_introselect(
    rt_lightbaker3d *baker, int32_t first, int32_t count, int32_t nth, int axis) {
    int32_t lo = first;
    int32_t hi = first + count;
    int32_t depth_limit = 0;
    for (int32_t n = count; n > 1; n /= 2)
        depth_limit += 2;
    while (hi - lo > 1) {
        int32_t mid;
        int32_t pivot_at;
        int32_t pivot;
        int32_t store_at;
        if (depth_limit-- <= 0) {
            baker_heap_sort(baker, lo, hi - lo, axis);
            return;
        }
        mid = lo + (hi - lo) / 2;
        if (baker_centroid_compare(baker, baker->bvh_order[mid], baker->bvh_order[lo], axis) < 0)
            baker_order_swap(&baker->bvh_order[mid], &baker->bvh_order[lo]);
        if (baker_centroid_compare(baker, baker->bvh_order[hi - 1], baker->bvh_order[mid], axis) <
            0)
            baker_order_swap(&baker->bvh_order[hi - 1], &baker->bvh_order[mid]);
        if (baker_centroid_compare(baker, baker->bvh_order[mid], baker->bvh_order[lo], axis) < 0)
            baker_order_swap(&baker->bvh_order[mid], &baker->bvh_order[lo]);
        pivot_at = mid;
        pivot = baker->bvh_order[pivot_at];
        baker_order_swap(&baker->bvh_order[pivot_at], &baker->bvh_order[hi - 1]);
        store_at = lo;
        for (int32_t i = lo; i < hi - 1; ++i) {
            if (baker_centroid_compare(baker, baker->bvh_order[i], pivot, axis) < 0) {
                baker_order_swap(&baker->bvh_order[i], &baker->bvh_order[store_at]);
                store_at++;
            }
        }
        baker_order_swap(&baker->bvh_order[store_at], &baker->bvh_order[hi - 1]);
        if (store_at == nth)
            return;
        if (nth < store_at)
            hi = store_at;
        else
            lo = store_at + 1;
    }
}

/// @brief Recursively build a deterministic median-split BVH subtree.
/// @details Bounds the indexed range, emits leaves containing at most four
///   triangles, and uses introselect on the longest centroid axis before
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
    node->min_b[0] = node->min_b[1] = node->min_b[2] = DBL_MAX;
    node->max_b[0] = node->max_b[1] = node->max_b[2] = -DBL_MAX;
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
    int32_t half = count / 2;
    baker_introselect(baker, first, count, first + half, axis);
    int32_t left = baker_bvh_build_recurse(baker, first, half);
    if (left < 0)
        return -1;
    int32_t right = baker_bvh_build_recurse(baker, first + half, count - half);
    if (right < 0)
        return -1;
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
    int32_t *order;
    baker_bvh_node *nodes;
    if (!baker || baker->tri_count < 0 || (baker->tri_count > 0 && !baker->tris))
        return 0;
    if (baker->tri_count == 0) {
        free(baker->bvh_order);
        free(baker->bvh_nodes);
        baker->bvh_order = NULL;
        baker->bvh_nodes = NULL;
        baker->bvh_node_count = 0;
        return 1;
    }
    if (baker->bvh_order && baker->bvh_nodes && baker->bvh_node_count > 0)
        return 1;
    free(baker->bvh_order);
    free(baker->bvh_nodes);
    baker->bvh_order = NULL;
    baker->bvh_nodes = NULL;
    baker->bvh_node_count = 0;
    order = (int32_t *)malloc((size_t)baker->tri_count * sizeof(*order));
    nodes = (baker_bvh_node *)malloc(((size_t)baker->tri_count * 2 + 1) * sizeof(*nodes));
    if (!order || !nodes) {
        free(order);
        free(nodes);
        return 0;
    }
    baker->bvh_order = order;
    baker->bvh_nodes = nodes;
    for (int32_t i = 0; i < baker->tri_count; ++i)
        baker->bvh_order[i] = i;
    if (baker_bvh_build_recurse(baker, 0, baker->tri_count) != 0) {
        free(baker->bvh_order);
        free(baker->bvh_nodes);
        baker->bvh_order = NULL;
        baker->bvh_nodes = NULL;
        baker->bvh_node_count = 0;
        return 0;
    }
    return 1;
}

/// @brief Test a ray interval against an axis-aligned BVH node.
/// @param origin Three-element ray origin.
/// @param dir Three-element ray direction.
/// @param min_b Three-element box minimum.
/// @param max_b Three-element box maximum.
/// @param t_max Exclusive working maximum distance.
/// @param t_near_out Optional output receiving the nonnegative box entry distance.
/// @return Nonzero when the forward interval intersects before @p t_max.
static int baker_ray_aabb(const double origin[3],
                          const double dir[3],
                          const double min_b[3],
                          const double max_b[3],
                          double t_max,
                          double *t_near_out) {
    double t0 = 0.0, t1 = t_max;
    if (!origin || !dir || !min_b || !max_b || !isfinite(t_max) || t_max <= 0.0)
        return 0;
    for (int a = 0; a < 3; ++a) {
        double near_t;
        double far_t;
        if (!isfinite(origin[a]) || !isfinite(dir[a]) || !isfinite(min_b[a]) || !isfinite(max_b[a]))
            return 0;
        if (dir[a] == 0.0) {
            if (origin[a] < min_b[a] || origin[a] > max_b[a])
                return 0;
            continue;
        }
        near_t = (min_b[a] - origin[a]) / dir[a];
        far_t = (max_b[a] - origin[a]) / dir[a];
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
    if (t_near_out)
        *t_near_out = t0;
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
    if (!isfinite(det) || fabs(det) < 1e-12)
        return 0;
    double inv_det = 1.0 / det;
    double tv[3] = {origin[0] - tri->p0[0], origin[1] - tri->p0[1], origin[2] - tri->p0[2]};
    double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv_det;
    if (!isfinite(u) || u < -1e-9 || u > 1.0 + 1e-9)
        return 0;
    double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1],
                    tv[2] * e1[0] - tv[0] * e1[2],
                    tv[0] * e1[1] - tv[1] * e1[0]};
    double v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv_det;
    if (!isfinite(v) || v < -1e-9 || u + v > 1.0 + 1e-9)
        return 0;
    double t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv_det;
    if (!isfinite(t) || t <= BAKER3D_EPS)
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

    typedef struct {
        int32_t node_index;
        double t_near;
    } baker_trace_stack_entry;

    baker_trace_stack_entry stack[64];
    int32_t stack_count = 0;
    double root_near;
    if (!baker_ray_aabb(
            origin, dir, baker->bvh_nodes[0].min_b, baker->bvh_nodes[0].max_b, t_max, &root_near))
        return -1;
    stack[stack_count++] = (baker_trace_stack_entry){0, root_near};
    int32_t best = -1;
    double best_t = t_max;
    while (stack_count > 0) {
        baker_trace_stack_entry entry = stack[--stack_count];
        const baker_bvh_node *node = &baker->bvh_nodes[entry.node_index];
        if (entry.t_near >= best_t)
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
        } else {
            const baker_bvh_node *left = &baker->bvh_nodes[node->left];
            const baker_bvh_node *right = &baker->bvh_nodes[node->right];
            double left_near;
            double right_near;
            int left_hit =
                baker_ray_aabb(origin, dir, left->min_b, left->max_b, best_t, &left_near);
            int right_hit =
                baker_ray_aabb(origin, dir, right->min_b, right->max_b, best_t, &right_near);
            if (left_hit && right_hit && stack_count + 2 <= 64) {
                if (left_near <= right_near) {
                    stack[stack_count++] = (baker_trace_stack_entry){node->right, right_near};
                    stack[stack_count++] = (baker_trace_stack_entry){node->left, left_near};
                } else {
                    stack[stack_count++] = (baker_trace_stack_entry){node->left, left_near};
                    stack[stack_count++] = (baker_trace_stack_entry){node->right, right_near};
                }
            } else if (left_hit && stack_count < 64) {
                stack[stack_count++] = (baker_trace_stack_entry){node->left, left_near};
            } else if (right_hit && stack_count < 64) {
                stack[stack_count++] = (baker_trace_stack_entry){node->right, right_near};
            }
        }
    }
    if (best >= 0 && t_out)
        *t_out = best_t;
    return best;
}

/*==========================================================================
 * Radiance estimation
 *=========================================================================*/

/// @brief Evaluate finite distance decay using the realtime light contract.
static double baker_light_distance_decay(const baker_light *light, double distance) {
    double powered_distance;
    double denominator;
    if (!light || !isfinite(distance) || distance < 0.0)
        return 0.0;
    if (light->decay_type == 0)
        return 1.0;
    powered_distance = distance;
    if (light->decay_type >= 2)
        powered_distance *= distance;
    if (light->decay_type >= 3)
        powered_distance *= distance;
    if (!isfinite(powered_distance))
        return 0.0;
    denominator = 1.0 + light->attenuation * powered_distance;
    if (!isfinite(denominator) || denominator <= 0.0)
        return 0.0;
    return 1.0 / denominator;
}

/// @brief Smoothly fade a local light to zero at its positive authored range.
static double baker_light_range_fade(double distance, double range) {
    double remaining;
    if (!isfinite(range) || range <= 0.0)
        return 1.0;
    if (!isfinite(distance) || distance >= range)
        return 0.0;
    remaining = 1.0 - distance / range;
    return remaining * remaining * (3.0 - 2.0 * remaining);
}

/// @brief Normalize one finite three-vector in place.
/// @param value Mutable vector.
/// @return Nonzero when a finite unit vector was produced.
static int baker_normalize3(double value[3]) {
    double length;
    if (!value)
        return 0;
    length = hypot(hypot(value[0], value[1]), value[2]);
    if (!isfinite(length) || length <= 1e-12)
        return 0;
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
    return 1;
}

/// @brief Evaluate one copied analytic light at a world-space point.
/// @param light Immutable light snapshot.
/// @param point World-space shading point.
/// @param to_light Output unit direction for surface lights.
/// @param distance Output distance to the selected emitter point, or `DBL_MAX` for directional.
/// @param attenuation Output finite nonnegative attenuation.
/// @return Zero for no contribution, one for directional surface irradiance, or two for isotropic
/// volume irradiance.
static int baker_evaluate_light(const baker_light *light,
                                const double point[3],
                                double to_light[3],
                                double *distance,
                                double *attenuation) {
    double dist = DBL_MAX;
    double atten = 1.0;
    if (!light || !point || !to_light || !distance || !attenuation)
        return 0;
    if (light->type == 0) {
        to_light[0] = -light->direction[0];
        to_light[1] = -light->direction[1];
        to_light[2] = -light->direction[2];
        if (!baker_normalize3(to_light))
            return 0;
    } else if (light->type == 1 || light->type == 3) {
        to_light[0] = light->position[0] - point[0];
        to_light[1] = light->position[1] - point[1];
        to_light[2] = light->position[2] - point[2];
        dist = hypot(hypot(to_light[0], to_light[1]), to_light[2]);
        if (!isfinite(dist) || dist <= 1e-9)
            return 0;
        to_light[0] /= dist;
        to_light[1] /= dist;
        to_light[2] /= dist;
        atten =
            baker_light_distance_decay(light, dist) * baker_light_range_fade(dist, light->range);
        if (light->type == 3) {
            double spot_dot = to_light[0] * -light->direction[0] +
                              to_light[1] * -light->direction[1] +
                              to_light[2] * -light->direction[2];
            if (spot_dot < light->outer_cos)
                return 0;
            if (spot_dot < light->inner_cos) {
                double cone_range = light->inner_cos - light->outer_cos;
                double t;
                if (cone_range <= 1e-12)
                    return 0;
                t = (spot_dot - light->outer_cos) / cone_range;
                atten *= t * t * (3.0 - 2.0 * t);
            }
        }
    } else if (light->type == 4) {
        double basis_u[3] = {light->basis_u[0], light->basis_u[1], light->basis_u[2]};
        double basis_v[3] = {light->basis_v[0], light->basis_v[1], light->basis_v[2]};
        double relative[3] = {point[0] - light->position[0],
                              point[1] - light->position[1],
                              point[2] - light->position[2]};
        double half_width = fmax(light->width * 0.5, 1e-9);
        double half_height = fmax(light->height * 0.5, 1e-9);
        double u;
        double v;
        double emitter_cosine;
        double area;
        double solid_angle;
        if (!baker_normalize3(basis_u) || !baker_normalize3(basis_v))
            return 0;
        u = relative[0] * basis_u[0] + relative[1] * basis_u[1] + relative[2] * basis_u[2];
        v = relative[0] * basis_v[0] + relative[1] * basis_v[1] + relative[2] * basis_v[2];
        u = fmax(-half_width, fmin(half_width, u));
        v = fmax(-half_height, fmin(half_height, v));
        to_light[0] = light->position[0] + basis_u[0] * u + basis_v[0] * v - point[0];
        to_light[1] = light->position[1] + basis_u[1] * u + basis_v[1] * v - point[1];
        to_light[2] = light->position[2] + basis_u[2] * u + basis_v[2] * v - point[2];
        dist = hypot(hypot(to_light[0], to_light[1]), to_light[2]);
        if (!isfinite(dist))
            return 0;
        if (dist <= 1e-9) {
            to_light[0] = -light->direction[0];
            to_light[1] = -light->direction[1];
            to_light[2] = -light->direction[2];
            if (!baker_normalize3(to_light))
                return 0;
            dist = 0.0;
        } else {
            to_light[0] /= dist;
            to_light[1] /= dist;
            to_light[2] /= dist;
        }
        emitter_cosine = -(light->direction[0] * to_light[0] + light->direction[1] * to_light[1] +
                           light->direction[2] * to_light[2]);
        if (!isfinite(emitter_cosine) || emitter_cosine <= 0.0)
            return 0;
        area = fmax(light->width * light->height, 1e-12);
        solid_angle = area / (area + 3.14159265358979323846 * dist * dist);
        atten = fmin(emitter_cosine, 1.0) * solid_angle * baker_light_distance_decay(light, dist) *
                baker_light_range_fade(dist, light->range);
    } else if (light->type == 5) {
        double radius = fmax(light->radius, 1e-9);
        double center_distance;
        double solid_angle;
        to_light[0] = light->position[0] - point[0];
        to_light[1] = light->position[1] - point[1];
        to_light[2] = light->position[2] - point[2];
        center_distance = hypot(hypot(to_light[0], to_light[1]), to_light[2]);
        if (!isfinite(center_distance))
            return 0;
        if (center_distance <= 1e-9) {
            to_light[0] = 0.0;
            to_light[1] = 1.0;
            to_light[2] = 0.0;
            dist = 0.0;
        } else {
            to_light[0] /= center_distance;
            to_light[1] /= center_distance;
            to_light[2] /= center_distance;
            dist = center_distance > radius ? center_distance - radius : 0.0;
        }
        solid_angle = radius * radius / (radius * radius + dist * dist);
        atten = solid_angle * baker_light_distance_decay(light, dist) *
                baker_light_range_fade(dist, light->range);
    } else if (light->type == 6) {
        double radius = fmax(light->radius, 1e-9);
        double t;
        dist = hypot(hypot(point[0] - light->position[0], point[1] - light->position[1]),
                     point[2] - light->position[2]);
        if (!isfinite(dist) || dist >= radius)
            return 0;
        t = 1.0 - dist / radius;
        *distance = dist;
        *attenuation = t * t * (3.0 - 2.0 * t);
        to_light[0] = to_light[1] = to_light[2] = 0.0;
        return 2;
    } else {
        return 0;
    }
    if (!isfinite(atten) || atten <= 0.0)
        return 0;
    *distance = dist;
    *attenuation = atten;
    return 1;
}

/// @brief Compute a conservative finite influence AABB for one copied light.
/// @return Nonzero for spatially bounded lights, or zero when the light must remain global.
static int baker_light_bounds(const baker_light *light, double min_b[3], double max_b[3]) {
    double extent;
    if (!light || light->type == 0)
        return 0;
    if (light->type == 6) {
        extent = light->radius;
    } else {
        if (!(light->range > 0.0) || !isfinite(light->range))
            return 0;
        extent = light->range;
        if (light->type == 4)
            extent += hypot(light->width, light->height) * 0.5;
        else if (light->type == 5)
            extent += light->radius;
    }
    if (!(extent > 0.0) || !isfinite(extent))
        return 0;
    for (int axis = 0; axis < 3; ++axis) {
        min_b[axis] = light->position[axis] - extent;
        max_b[axis] = light->position[axis] + extent;
        if (!isfinite(min_b[axis]) || !isfinite(max_b[axis]))
            return 0;
    }
    return 1;
}

static double baker_light_center_axis(const rt_lightbaker3d *baker, int32_t light_index, int axis) {
    return baker->lights[light_index].position[axis];
}

/// @brief Build one balanced light-influence BVH node by midpoint partitioning.
static int32_t baker_light_bvh_build_node(rt_lightbaker3d *baker, int32_t first, int32_t count) {
    int32_t node_index = baker->light_bvh_node_count++;
    baker_light_bvh_node *node = &baker->light_bvh_nodes[node_index];
    double center_min[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
    double center_max[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    node->left = node->right = -1;
    node->first = first;
    node->count = count;
    for (int axis = 0; axis < 3; ++axis) {
        node->min_b[axis] = DBL_MAX;
        node->max_b[axis] = -DBL_MAX;
    }
    for (int32_t offset = 0; offset < count; ++offset) {
        int32_t light_index = baker->bounded_light_order[first + offset];
        double min_b[3], max_b[3];
        (void)baker_light_bounds(&baker->lights[light_index], min_b, max_b);
        for (int axis = 0; axis < 3; ++axis) {
            double center = baker_light_center_axis(baker, light_index, axis);
            if (min_b[axis] < node->min_b[axis])
                node->min_b[axis] = min_b[axis];
            if (max_b[axis] > node->max_b[axis])
                node->max_b[axis] = max_b[axis];
            if (center < center_min[axis])
                center_min[axis] = center;
            if (center > center_max[axis])
                center_max[axis] = center;
        }
    }
    if (count <= 8)
        return node_index;
    int axis = 0;
    if (center_max[1] - center_min[1] > center_max[axis] - center_min[axis])
        axis = 1;
    if (center_max[2] - center_min[2] > center_max[axis] - center_min[axis])
        axis = 2;
    double pivot = 0.5 * (center_min[axis] + center_max[axis]);
    int32_t lower = first;
    int32_t upper = first + count - 1;
    while (lower <= upper) {
        int32_t light_index = baker->bounded_light_order[lower];
        if (baker_light_center_axis(baker, light_index, axis) < pivot) {
            ++lower;
        } else {
            int32_t temporary = baker->bounded_light_order[lower];
            baker->bounded_light_order[lower] = baker->bounded_light_order[upper];
            baker->bounded_light_order[upper--] = temporary;
        }
    }
    if (lower == first || lower == first + count)
        lower = first + count / 2;
    node->left = baker_light_bvh_build_node(baker, first, lower - first);
    node->right = baker_light_bvh_build_node(baker, lower, first + count - lower);
    node->count = 0;
    return node_index;
}

/// @brief Build immutable global and spatially bounded light lists for hot sample queries.
static int baker_build_light_index(rt_lightbaker3d *baker) {
    if (!baker || baker->light_count < 0)
        return 0;
    if (baker->light_index_built)
        return 1;
    if (baker->light_count == 0) {
        baker->light_index_built = 1;
        return 1;
    }
    baker->bounded_light_order =
        (int32_t *)malloc((size_t)baker->light_count * sizeof(*baker->bounded_light_order));
    baker->global_light_indices =
        (int32_t *)malloc((size_t)baker->light_count * sizeof(*baker->global_light_indices));
    baker->light_bvh_nodes = (baker_light_bvh_node *)calloc((size_t)baker->light_count * 2u,
                                                            sizeof(*baker->light_bvh_nodes));
    if (!baker->bounded_light_order || !baker->global_light_indices || !baker->light_bvh_nodes)
        return 0;
    for (int32_t index = 0; index < baker->light_count; ++index) {
        double min_b[3], max_b[3];
        if (baker_light_bounds(&baker->lights[index], min_b, max_b))
            baker->bounded_light_order[baker->bounded_light_count++] = index;
        else
            baker->global_light_indices[baker->global_light_count++] = index;
    }
    if (baker->bounded_light_count > 0)
        (void)baker_light_bvh_build_node(baker, 0, baker->bounded_light_count);
    baker->light_index_built = 1;
    return 1;
}

/// @brief Accumulate one indexed light after spatial candidate selection.
static void baker_accumulate_direct_light(const rt_lightbaker3d *baker,
                                          int32_t light_index,
                                          const double point[3],
                                          const double normal[3],
                                          double out_rgb[3]) {
    const baker_light *light = &baker->lights[light_index];
    double to_light[3];
    double dist;
    double atten;
    int mode = baker_evaluate_light(light, point, to_light, &dist, &atten);
    if (mode == 0)
        return;
    if (mode == 2) {
        double volume_scale = atten * light->intensity;
        out_rgb[0] += light->color[0] * volume_scale;
        out_rgb[1] += light->color[1] * volume_scale;
        out_rgb[2] += light->color[2] * volume_scale;
        return;
    }
    double ndl = normal[0] * to_light[0] + normal[1] * to_light[1] + normal[2] * to_light[2];
    if (ndl <= 0.0)
        return;
    double origin[3] = {point[0] + normal[0] * BAKER3D_EPS * 4,
                        point[1] + normal[1] * BAKER3D_EPS * 4,
                        point[2] + normal[2] * BAKER3D_EPS * 4};
    if (light->casts_shadows) {
        double shadow_limit = dist == DBL_MAX ? DBL_MAX : dist - BAKER3D_EPS * 8;
        double t;
        if (shadow_limit > BAKER3D_EPS &&
            baker_trace(baker, origin, to_light, shadow_limit, &t) >= 0)
            return;
    }
    double scale = ndl * atten * light->intensity;
    out_rgb[0] += light->color[0] * scale;
    out_rgb[1] += light->color[1] * scale;
    out_rgb[2] += light->color[2] * scale;
}

/// @brief Accumulate direct irradiance from the baker's immutable light snapshot.
/// @details Directional lights use parallel rays; local lights honor finite
///   range and authored decay, spot lights apply smooth cone falloff, and only
///   lights that request shadows submit BVH occlusion rays.
/// @param baker Baker containing copied lights and trace geometry.
/// @param point Three-element world-space surface point.
/// @param normal Three-element oriented surface normal.
/// @param out_rgb Output three-element irradiance initialized to black.
static void baker_direct_light(const rt_lightbaker3d *baker,
                               const double point[3],
                               const double normal[3],
                               double out_rgb[3]) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0;
    for (int32_t global = 0; global < baker->global_light_count; ++global)
        baker_accumulate_direct_light(
            baker, baker->global_light_indices[global], point, normal, out_rgb);
    if (baker->light_bvh_node_count > 0) {
        int32_t stack[64];
        int32_t stack_count = 0;
        stack[stack_count++] = 0;
        while (stack_count > 0) {
            const baker_light_bvh_node *node = &baker->light_bvh_nodes[stack[--stack_count]];
            if (point[0] < node->min_b[0] || point[0] > node->max_b[0] ||
                point[1] < node->min_b[1] || point[1] > node->max_b[1] ||
                point[2] < node->min_b[2] || point[2] > node->max_b[2])
                continue;
            if (node->count > 0) {
                for (int32_t offset = 0; offset < node->count; ++offset)
                    baker_accumulate_direct_light(baker,
                                                  baker->bounded_light_order[node->first + offset],
                                                  point,
                                                  normal,
                                                  out_rgb);
            } else if (stack_count <= 62) {
                stack[stack_count++] = node->left;
                stack[stack_count++] = node->right;
            }
        }
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
/// @param include_direct Include direct light at this surface (recursive hits always do).
/// @param out_rgb Output three-element irradiance.
static void baker_radiance(const rt_lightbaker3d *baker,
                           const double point[3],
                           const double normal[3],
                           uint32_t *rng,
                           int32_t bounces,
                           int include_direct,
                           double out_rgb[3]) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0;
    if (include_direct)
        baker_direct_light(baker, point, normal, out_rgb);
    if (bounces <= 0)
        return;
    double origin[3] = {point[0] + normal[0] * BAKER3D_EPS * 4,
                        point[1] + normal[1] * BAKER3D_EPS * 4,
                        point[2] + normal[2] * BAKER3D_EPS * 4};
    double dir[3];
    baker_cosine_dir(rng, normal, dir);
    double t;
    int32_t hit = baker_trace(baker, origin, dir, DBL_MAX, &t);
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
    baker_radiance(baker, hp, hn, rng, bounces - 1, 1, bounce_rgb);
    /* Cosine-weighted sampling folds the ndl/pdf terms; albedo modulates. */
    out_rgb[0] += (bounce_rgb[0] * tri->albedo[0] + tri->emissive[0]);
    out_rgb[1] += (bounce_rgb[1] * tri->albedo[1] + tri->emissive[1]);
    out_rgb[2] += (bounce_rgb[2] * tri->albedo[2] + tri->emissive[2]);
}

typedef struct baker_sample_task {
    const rt_lightbaker3d *baker;
    double point[3];
    double normal[3];
    double (*results)[3];
    int64_t first_sample;
    int32_t sample_count;
    int32_t result_offset;
    int32_t tri_index;
    int32_t texel_x;
    int32_t texel_y;
    int32_t bounces;
} baker_sample_task;

static void *baker_worker_pool(rt_lightbaker3d *baker) {
    int64_t workers;
    if (!baker || baker->worker_pool_failed || rt_threadpool_current_worker_pool())
        return NULL;
    if (baker->worker_pool)
        return baker->worker_pool;
    workers = rt_parallel_default_workers();
    if (workers > BAKER3D_MAX_PARALLEL_TASKS)
        workers = BAKER3D_MAX_PARALLEL_TASKS;
    if (workers < 2) {
        baker->worker_pool_failed = 1;
        return NULL;
    }
    baker->worker_pool = rt_threadpool_new(workers);
    if (!baker->worker_pool) {
        baker->worker_pool_failed = 1;
        return NULL;
    }
    baker->worker_count = (int32_t)workers;
    return baker->worker_pool;
}

/// @brief Derive an independent deterministic path seed from immutable sample coordinates.
static uint32_t baker_sample_seed(int32_t tri_index,
                                  int32_t texel_x,
                                  int32_t texel_y,
                                  int64_t sample_index) {
    uint64_t value = (uint64_t)(uint32_t)tri_index * UINT64_C(0x9e3779b185ebca87);
    value ^= (uint64_t)(uint32_t)texel_x * UINT64_C(0xc2b2ae3d27d4eb4f);
    value ^= (uint64_t)(uint32_t)texel_y * UINT64_C(0x165667b19e3779f9);
    value ^= (uint64_t)sample_index * UINT64_C(0x85ebca77c2b2ae63);
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return (uint32_t)(value ^ (value >> 32)) | 1u;
}

static void baker_sample_task_run(void *argument) {
    baker_sample_task *task = (baker_sample_task *)argument;
    if (!task || !task->baker || !task->results || task->sample_count <= 0)
        return;
    for (int32_t offset = 0; offset < task->sample_count; ++offset) {
        uint32_t rng = baker_sample_seed(
            task->tri_index, task->texel_x, task->texel_y, task->first_sample + offset);
        baker_radiance(task->baker,
                       task->point,
                       task->normal,
                       &rng,
                       task->bounces,
                       task->baker->include_direct,
                       task->results[task->result_offset + offset]);
    }
}

/// @brief Evaluate one bounded sample batch in parallel without changing reduction order.
static void baker_evaluate_sample_batch(rt_lightbaker3d *baker,
                                        const baker_tri *tri,
                                        int32_t tri_index,
                                        int32_t texel_x,
                                        int32_t texel_y,
                                        const double point[3],
                                        int64_t first_sample,
                                        int32_t sample_count,
                                        double results[BAKER3D_SAMPLE_WORK_PER_STEP][3]) {
    baker_sample_task tasks[BAKER3D_MAX_PARALLEL_TASKS];
    void *pool = NULL;
    int32_t task_count = 1;
    if (!baker || !tri || !point || !results || sample_count <= 0)
        return;
    if (sample_count >= 8) {
        pool = baker_worker_pool(baker);
        int32_t workers = pool ? baker->worker_count : 1;
        if (workers > sample_count)
            workers = sample_count;
        if (workers > 1)
            task_count = workers;
    }
    int32_t base_count = sample_count / task_count;
    int32_t extra = sample_count % task_count;
    int32_t result_offset = 0;
    for (int32_t task_index = 0; task_index < task_count; ++task_index) {
        baker_sample_task *task = &tasks[task_index];
        memset(task, 0, sizeof(*task));
        task->baker = baker;
        memcpy(task->point, point, sizeof(task->point));
        memcpy(task->normal, tri->normal, sizeof(task->normal));
        task->results = results;
        task->first_sample = first_sample + result_offset;
        task->sample_count = base_count + (task_index < extra ? 1 : 0);
        task->result_offset = result_offset;
        task->tri_index = tri_index;
        task->texel_x = texel_x;
        task->texel_y = texel_y;
        task->bounces = (int32_t)baker->bounces;
        result_offset += task->sample_count;
        if (!pool || !rt_threadpool_submit_fn(pool, baker_sample_task_run, task))
            baker_sample_task_run(task);
    }
    if (pool)
        rt_threadpool_wait(pool);
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
    baker_release_node_entries(baker->nodes, baker->node_count);
    baker->node_count = 0;
    if (baker->worker_pool) {
        rt_threadpool_shutdown(baker->worker_pool);
        baker_release_ref(&baker->worker_pool);
    }
    baker->worker_count = 0;
    free(baker->tris);
    free(baker->bvh_order);
    free(baker->bvh_nodes);
    free(baker->lights);
    free(baker->bounded_light_order);
    free(baker->global_light_indices);
    free(baker->light_bvh_nodes);
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
    baker->include_direct = 1;
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

/// @brief Return whether bake inputs may still be changed without mixing slices.
static int baker_inputs_mutable(const rt_lightbaker3d *baker) {
    return baker && !baker->gathered && !baker->done;
}

/// @brief Set the chart-density target for subsequent bake steps.
/// @param obj LightBaker3D receiver; invalid handles report a trap.
/// @param texels Positive texels per world unit, capped at 64; other values are ignored.
void rt_lightbaker3d_set_texels_per_unit(void *obj, double texels) {
    rt_lightbaker3d *baker =
        lightbaker3d_checked(obj, "LightBaker3D.set_TexelsPerUnit: invalid baker");
    if (baker_inputs_mutable(baker) && isfinite(texels) && texels > 0.0)
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
    if (baker_inputs_mutable(baker) && samples > 0)
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
    if (baker_inputs_mutable(baker) && bounces >= 0)
        baker->bounces = bounces > 8 ? 8 : bounces;
}

/// @brief Return the configured indirect bounce depth.
/// @param obj LightBaker3D receiver.
/// @return Bounce count, or zero after invalid-handle reporting.
int64_t rt_lightbaker3d_get_bounces(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.get_Bounces: invalid baker");
    return baker ? baker->bounces : 0;
}

/// @brief Configure primary-surface direct light before gathering freezes inputs.
/// @param obj LightBaker3D receiver.
/// @param enabled Nonzero includes direct light in lightmaps; probe grids are unchanged.
void rt_lightbaker3d_set_include_direct(void *obj, int8_t enabled) {
    rt_lightbaker3d *baker =
        lightbaker3d_checked(obj, "LightBaker3D.set_IncludeDirect: invalid baker");
    if (baker_inputs_mutable(baker))
        baker->include_direct = enabled != 0;
}

/// @brief Return the primary-surface direct-light option.
/// @param obj LightBaker3D receiver.
/// @return Normalized Boolean, or zero after an invalid-receiver trap.
int8_t rt_lightbaker3d_get_include_direct(void *obj) {
    rt_lightbaker3d *baker =
        lightbaker3d_checked(obj, "LightBaker3D.get_IncludeDirect: invalid baker");
    return baker ? baker->include_direct : 0;
}

/// @brief Set nonnegative radiance returned by rays that miss the scene.
/// @param obj LightBaker3D receiver.
/// @param r Red sky radiance; nonfinite or nonpositive input becomes zero.
/// @param g Green sky radiance; nonfinite or nonpositive input becomes zero.
/// @param b Blue sky radiance; nonfinite or nonpositive input becomes zero.
void rt_lightbaker3d_set_sky_color(void *obj, double r, double g, double b) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.SetSkyColor: invalid baker");
    if (!baker_inputs_mutable(baker))
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

/// @brief Ensure dynamic light-snapshot capacity for one more input.
/// @param baker Mutable baker whose light table may grow.
/// @return Nonzero when one more light can be appended, otherwise zero.
static int baker_reserve_light(rt_lightbaker3d *baker) {
    int32_t new_capacity;
    baker_light *grown;
    if (!baker || baker->light_count < 0 || baker->light_capacity < 0 ||
        baker->light_count > baker->light_capacity || baker->light_count == INT32_MAX)
        return 0;
    if (baker->light_count < baker->light_capacity)
        return 1;
    new_capacity = baker->light_capacity > 0 ? baker->light_capacity : 16;
    if (new_capacity <= INT32_MAX / 2)
        new_capacity *= 2;
    else
        new_capacity = baker->light_count + 1;
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*grown))
        return 0;
    grown = (baker_light *)realloc(baker->lights, (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return 0;
    baker->lights = grown;
    baker->light_capacity = new_capacity;
    return 1;
}

/// @brief Snapshot one enabled non-ambient Light3D into the bake input.
/// @details Copies and sanitizes the complete analytic-light state immediately;
///   the baker does not retain the source light and later mutations do not affect
///   this bake. Storage grows dynamically so accepted lights are never silently dropped.
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
    double direction_length;
    if (!baker_inputs_mutable(baker) || light->type < 0 || light->type > 6 || light->type == 2 ||
        !light->enabled)
        return;
    if (light->type != 0 && (!isfinite(light->position[0]) || !isfinite(light->position[1]) ||
                             !isfinite(light->position[2])))
        return;
    if (!baker_reserve_light(baker)) {
        rt_trap("LightBaker3D.AddLight: memory allocation failed");
        return;
    }
    baker_light *slot = &baker->lights[baker->light_count];
    memset(slot, 0, sizeof(*slot));
    slot->type = light->type;
    memcpy(slot->direction, light->direction, sizeof(slot->direction));
    memcpy(slot->position, light->position, sizeof(slot->position));
    memcpy(slot->basis_u, light->basis_u, sizeof(slot->basis_u));
    memcpy(slot->basis_v, light->basis_v, sizeof(slot->basis_v));
    direction_length = hypot(hypot(slot->direction[0], slot->direction[1]), slot->direction[2]);
    if ((slot->type == 0 || slot->type == 3 || slot->type == 4) &&
        (!isfinite(direction_length) || direction_length <= 1e-12))
        return;
    if (isfinite(direction_length) && direction_length > 1e-12) {
        slot->direction[0] /= direction_length;
        slot->direction[1] /= direction_length;
        slot->direction[2] /= direction_length;
    }
    for (int axis = 0; axis < 3; ++axis) {
        double color = light->color[axis];
        slot->color[axis] = !isfinite(color) || color < 0.0 ? 0.0 : color > 1.0 ? 1.0 : color;
    }
    slot->intensity = isfinite(light->intensity) && light->intensity > 0.0 ? light->intensity : 0.0;
    slot->attenuation =
        isfinite(light->attenuation) && light->attenuation >= 0.0 ? light->attenuation : 0.001;
    slot->inner_cos = isfinite(light->inner_cos) ? fmax(-1.0, fmin(1.0, light->inner_cos)) : 1.0;
    slot->outer_cos = isfinite(light->outer_cos) ? fmax(-1.0, fmin(1.0, light->outer_cos)) : 0.0;
    if (slot->inner_cos < slot->outer_cos) {
        double tmp = slot->inner_cos;
        slot->inner_cos = slot->outer_cos;
        slot->outer_cos = tmp;
    }
    slot->range = isfinite(light->range) && light->range > 0.0 ? light->range : 0.0;
    slot->width = isfinite(light->width) && light->width > 0.0 ? light->width : 1.0;
    slot->height = isfinite(light->height) && light->height > 0.0 ? light->height : 1.0;
    slot->radius = isfinite(light->radius) && light->radius > 0.0 ? light->radius : 1.0;
    slot->decay_type = light->decay_type >= 0 && light->decay_type <= 3 ? light->decay_type : 2;
    slot->casts_shadows = light->casts_shadows ? 1 : 0;
    baker->light_count++;
}

typedef struct baker_chart_edge {
    int32_t node_index;
    uint32_t lo;
    uint32_t hi;
    int32_t tri_index;
} baker_chart_edge;

typedef struct baker_chart_rect {
    int32_t root;
    int32_t width;
    int32_t height;
} baker_chart_rect;

/// @brief Order topology edges so shared source-mesh edges become contiguous.
static int baker_chart_edge_compare(const void *lhs, const void *rhs) {
    const baker_chart_edge *a = (const baker_chart_edge *)lhs;
    const baker_chart_edge *b = (const baker_chart_edge *)rhs;
    if (a->node_index != b->node_index)
        return a->node_index < b->node_index ? -1 : 1;
    if (a->lo != b->lo)
        return a->lo < b->lo ? -1 : 1;
    if (a->hi != b->hi)
        return a->hi < b->hi ? -1 : 1;
    return a->tri_index < b->tri_index ? -1 : a->tri_index != b->tri_index;
}

/// @brief Order larger islands first for deterministic skyline packing.
static int baker_chart_rect_compare(const void *lhs, const void *rhs) {
    const baker_chart_rect *a = (const baker_chart_rect *)lhs;
    const baker_chart_rect *b = (const baker_chart_rect *)rhs;
    uint64_t area_a = (uint64_t)(uint32_t)a->width * (uint32_t)a->height;
    uint64_t area_b = (uint64_t)(uint32_t)b->width * (uint32_t)b->height;
    if (area_a != area_b)
        return area_a > area_b ? -1 : 1;
    if (a->height != b->height)
        return a->height > b->height ? -1 : 1;
    return a->root < b->root ? -1 : a->root != b->root;
}

static int32_t baker_chart_find_root(int32_t *parents, int32_t index) {
    int32_t root = index;
    while (parents[root] != root)
        root = parents[root];
    while (parents[index] != index) {
        int32_t next = parents[index];
        parents[index] = root;
        index = next;
    }
    return root;
}

static void baker_chart_union(int32_t *parents, int32_t a, int32_t b) {
    int32_t root_a = baker_chart_find_root(parents, a);
    int32_t root_b = baker_chart_find_root(parents, b);
    if (root_a == root_b)
        return;
    if (root_a < root_b)
        parents[root_b] = root_a;
    else
        parents[root_a] = root_b;
}

/// @brief Return barycentric coordinates for the center of one island-chart texel.
static int baker_chart_texel_barycentric(
    const baker_tri *tri, int32_t x, int32_t y, double *out_b1, double *out_b2) {
    double px = (double)x + 0.5;
    double py = (double)y + 0.5;
    double x10 = tri->chart_uv[1][0] - tri->chart_uv[0][0];
    double y10 = tri->chart_uv[1][1] - tri->chart_uv[0][1];
    double x20 = tri->chart_uv[2][0] - tri->chart_uv[0][0];
    double y20 = tri->chart_uv[2][1] - tri->chart_uv[0][1];
    double dx = px - tri->chart_uv[0][0];
    double dy = py - tri->chart_uv[0][1];
    double determinant = x10 * y20 - y10 * x20;
    double b1;
    double b2;
    double b0;
    if (!isfinite(determinant) || fabs(determinant) <= 1e-12)
        return 0;
    b1 = (dx * y20 - dy * x20) / determinant;
    b2 = (x10 * dy - y10 * dx) / determinant;
    b0 = 1.0 - b1 - b2;
    if (!isfinite(b0) || !isfinite(b1) || !isfinite(b2) || b0 < -1e-9 || b1 < -1e-9 || b2 < -1e-9)
        return 0;
    if (out_b1)
        *out_b1 = b1;
    if (out_b2)
        *out_b2 = b2;
    return 1;
}

static uint64_t baker_chart_triangle_texel_count(const baker_tri *tri) {
    uint64_t count = 0;
    for (int32_t y = tri->texel_min_y; y <= tri->texel_max_y; ++y)
        for (int32_t x = tri->texel_min_x; x <= tri->texel_max_x; ++x)
            if (baker_chart_texel_barycentric(tri, x, y, NULL, NULL))
                count++;
    return count;
}

/// @brief Place one padded rectangle at the lowest deterministic skyline position.
static int baker_chart_skyline_place(int32_t atlas_dim,
                                     int32_t *skyline,
                                     int32_t width,
                                     int32_t height,
                                     int32_t *out_x,
                                     int32_t *out_y) {
    int32_t padded_width = width + 1;
    int32_t padded_height = height + 1;
    int32_t best_x = -1;
    int32_t best_y = INT32_MAX;
    if (!skyline || !out_x || !out_y || width <= 0 || height <= 0 || padded_width > atlas_dim ||
        padded_height > atlas_dim)
        return 0;
    for (int32_t x = 0; x + padded_width <= atlas_dim; ++x) {
        int32_t y = 0;
        for (int32_t column = x; column < x + padded_width; ++column)
            if (skyline[column] > y)
                y = skyline[column];
        if (y + padded_height <= atlas_dim && y < best_y) {
            best_x = x;
            best_y = y;
        }
    }
    if (best_x < 0)
        return 0;
    for (int32_t column = best_x; column < best_x + padded_width; ++column)
        skyline[column] = best_y + padded_height;
    *out_x = best_x;
    *out_y = best_y;
    return 1;
}

/// @brief Build connected coplanar UV islands and pack them into the atlas skyline.
/// @details Triangles join only through a shared source-mesh edge and a near-coplanar normal,
///   preserving hard seams while eliminating redundant padding between ordinary quad faces.
static int baker_preflight_charts(rt_lightbaker3d *baker) {
    baker_chart_edge *edges = NULL;
    baker_chart_rect *rects = NULL;
    int32_t *parents = NULL;
    int32_t *root_width = NULL;
    int32_t *root_height = NULL;
    int32_t *root_x = NULL;
    int32_t *root_y = NULL;
    int32_t *skyline = NULL;
    double (*basis_u)[3] = NULL;
    double (*basis_v)[3] = NULL;
    double (*bounds)[4] = NULL;
    int32_t rect_count = 0;
    int result = 0;
    int32_t tri_count;
    uint64_t samples;
    if (!baker || baker->tri_count < 0 || (baker->tri_count > 0 && !baker->tris))
        return 0;
    tri_count = baker->tri_count;
    samples = (uint64_t)(baker->samples < 1 ? 1 : baker->samples);
    if (tri_count == 0)
        return 1;
    if ((size_t)tri_count > SIZE_MAX / (3u * sizeof(*edges)))
        return 0;
    edges = (baker_chart_edge *)malloc((size_t)tri_count * 3u * sizeof(*edges));
    rects = (baker_chart_rect *)malloc((size_t)tri_count * sizeof(*rects));
    parents = (int32_t *)malloc((size_t)tri_count * sizeof(*parents));
    root_width = (int32_t *)calloc((size_t)tri_count, sizeof(*root_width));
    root_height = (int32_t *)calloc((size_t)tri_count, sizeof(*root_height));
    root_x = (int32_t *)calloc((size_t)tri_count, sizeof(*root_x));
    root_y = (int32_t *)calloc((size_t)tri_count, sizeof(*root_y));
    basis_u = (double (*)[3])calloc((size_t)tri_count, sizeof(*basis_u));
    basis_v = (double (*)[3])calloc((size_t)tri_count, sizeof(*basis_v));
    bounds = (double (*)[4])malloc((size_t)tri_count * sizeof(*bounds));
    skyline = (int32_t *)calloc((size_t)baker->atlas_dim, sizeof(*skyline));
    if (!edges || !rects || !parents || !root_width || !root_height || !root_x || !root_y ||
        !basis_u || !basis_v || !bounds || !skyline)
        goto cleanup;
    for (int32_t i = 0; i < tri_count; ++i) {
        parents[i] = i;
        bounds[i][0] = bounds[i][1] = DBL_MAX;
        bounds[i][2] = bounds[i][3] = -DBL_MAX;
        for (int edge = 0; edge < 3; ++edge) {
            uint32_t a = baker->tris[i].vertex_indices[edge];
            uint32_t b = baker->tris[i].vertex_indices[(edge + 1) % 3];
            baker_chart_edge *record = &edges[(size_t)i * 3u + (size_t)edge];
            record->node_index = baker->tris[i].node_index;
            record->lo = a < b ? a : b;
            record->hi = a < b ? b : a;
            record->tri_index = i;
        }
    }
    qsort(edges, (size_t)tri_count * 3u, sizeof(*edges), baker_chart_edge_compare);
    for (int32_t first = 0; first < tri_count * 3;) {
        int32_t end = first + 1;
        while (end < tri_count * 3 && edges[end].node_index == edges[first].node_index &&
               edges[end].lo == edges[first].lo && edges[end].hi == edges[first].hi)
            end++;
        const baker_tri *representative = &baker->tris[edges[first].tri_index];
        for (int32_t b = first + 1; b < end; ++b) {
            const baker_tri *candidate = &baker->tris[edges[b].tri_index];
            double dot = representative->normal[0] * candidate->normal[0] +
                         representative->normal[1] * candidate->normal[1] +
                         representative->normal[2] * candidate->normal[2];
            if (isfinite(dot) && dot >= 0.9995)
                baker_chart_union(parents, edges[first].tri_index, edges[b].tri_index);
        }
        first = end;
    }

project_islands:
    memset(basis_u, 0, (size_t)tri_count * sizeof(*basis_u));
    memset(basis_v, 0, (size_t)tri_count * sizeof(*basis_v));
    for (int32_t i = 0; i < tri_count; ++i) {
        bounds[i][0] = bounds[i][1] = DBL_MAX;
        bounds[i][2] = bounds[i][3] = -DBL_MAX;
    }
    for (int32_t i = 0; i < tri_count; ++i) {
        int32_t root = baker_chart_find_root(parents, i);
        baker_tri *root_tri = &baker->tris[root];
        double axis[3] = {0.0, 0.0, 0.0};
        baker->tris[i].chart_root = root;
        if (root != i)
            continue;
        int least_axis = fabs(root_tri->normal[0]) <= fabs(root_tri->normal[1])
                             ? (fabs(root_tri->normal[0]) <= fabs(root_tri->normal[2]) ? 0 : 2)
                             : (fabs(root_tri->normal[1]) <= fabs(root_tri->normal[2]) ? 1 : 2);
        axis[least_axis] = 1.0;
        basis_u[root][0] = axis[1] * root_tri->normal[2] - axis[2] * root_tri->normal[1];
        basis_u[root][1] = axis[2] * root_tri->normal[0] - axis[0] * root_tri->normal[2];
        basis_u[root][2] = axis[0] * root_tri->normal[1] - axis[1] * root_tri->normal[0];
        double length = hypot(hypot(basis_u[root][0], basis_u[root][1]), basis_u[root][2]);
        if (!isfinite(length) || length <= 1e-12)
            goto cleanup;
        for (int component = 0; component < 3; ++component)
            basis_u[root][component] /= length;
        basis_v[root][0] =
            root_tri->normal[1] * basis_u[root][2] - root_tri->normal[2] * basis_u[root][1];
        basis_v[root][1] =
            root_tri->normal[2] * basis_u[root][0] - root_tri->normal[0] * basis_u[root][2];
        basis_v[root][2] =
            root_tri->normal[0] * basis_u[root][1] - root_tri->normal[1] * basis_u[root][0];
    }
    for (int32_t i = 0; i < tri_count; ++i) {
        int32_t root = baker->tris[i].chart_root;
        const double *positions[3] = {baker->tris[i].p0, baker->tris[i].p1, baker->tris[i].p2};
        for (int corner = 0; corner < 3; ++corner) {
            double u = positions[corner][0] * basis_u[root][0] +
                       positions[corner][1] * basis_u[root][1] +
                       positions[corner][2] * basis_u[root][2];
            double v = positions[corner][0] * basis_v[root][0] +
                       positions[corner][1] * basis_v[root][1] +
                       positions[corner][2] * basis_v[root][2];
            if (!isfinite(u) || !isfinite(v))
                goto cleanup;
            if (u < bounds[root][0])
                bounds[root][0] = u;
            if (v < bounds[root][1])
                bounds[root][1] = v;
            if (u > bounds[root][2])
                bounds[root][2] = u;
            if (v > bounds[root][3])
                bounds[root][3] = v;
        }
    }
    {
        int split_oversized_island = 0;
        for (int32_t root = 0; root < tri_count; ++root) {
            int has_other_triangle = 0;
            if (baker->tris[root].chart_root != root)
                continue;
            double scaled_w = (bounds[root][2] - bounds[root][0]) * baker->texels_per_unit;
            double scaled_h = (bounds[root][3] - bounds[root][1]) * baker->texels_per_unit;
            if (scaled_w < BAKER3D_MAX_CHART_DIM - 2 && scaled_h < BAKER3D_MAX_CHART_DIM - 2)
                continue;
            for (int32_t i = root + 1; i < tri_count; ++i)
                if (baker->tris[i].chart_root == root) {
                    has_other_triangle = 1;
                    break;
                }
            if (!has_other_triangle)
                continue;
            for (int32_t i = root; i < tri_count; ++i)
                if (baker->tris[i].chart_root == root)
                    parents[i] = i;
            split_oversized_island = 1;
        }
        if (split_oversized_island)
            goto project_islands;
    }
    for (int32_t root = 0; root < tri_count; ++root) {
        if (baker->tris[root].chart_root != root)
            continue;
        double scaled_w = (bounds[root][2] - bounds[root][0]) * baker->texels_per_unit;
        double scaled_h = (bounds[root][3] - bounds[root][1]) * baker->texels_per_unit;
        if (!isfinite(scaled_w) || !isfinite(scaled_h) || scaled_w < 0.0 || scaled_h < 0.0)
            goto cleanup;
        root_width[root] = scaled_w >= BAKER3D_MAX_CHART_DIM - 2 ? BAKER3D_MAX_CHART_DIM
                                                                 : (int32_t)ceil(scaled_w) + 2;
        root_height[root] = scaled_h >= BAKER3D_MAX_CHART_DIM - 2 ? BAKER3D_MAX_CHART_DIM
                                                                  : (int32_t)ceil(scaled_h) + 2;
        if (root_width[root] < 3)
            root_width[root] = 3;
        if (root_height[root] < 3)
            root_height[root] = 3;
        rects[rect_count++] = (baker_chart_rect){root, root_width[root], root_height[root]};
    }
    qsort(rects, (size_t)rect_count, sizeof(*rects), baker_chart_rect_compare);
    for (int32_t i = 0; i < rect_count; ++i)
        if (!baker_chart_skyline_place(baker->atlas_dim,
                                       skyline,
                                       rects[i].width,
                                       rects[i].height,
                                       &root_x[rects[i].root],
                                       &root_y[rects[i].root]))
            goto cleanup;
    baker->total_sample_work = 0;
    baker->completed_sample_work = 0;
    baker->next_texel = 0;
    baker->next_sample = 0;
    baker->sample_acc[0] = baker->sample_acc[1] = baker->sample_acc[2] = 0.0;
    for (int32_t i = 0; i < tri_count; ++i) {
        baker_tri *tri = &baker->tris[i];
        int32_t root = tri->chart_root;
        double extent_u = bounds[root][2] - bounds[root][0];
        double extent_v = bounds[root][3] - bounds[root][1];
        double scale_u = extent_u > 1e-12 ? (double)(root_width[root] - 2) / extent_u : 0.0;
        double scale_v = extent_v > 1e-12 ? (double)(root_height[root] - 2) / extent_v : 0.0;
        const double *positions[3] = {tri->p0, tri->p1, tri->p2};
        tri->chart_x = root_x[root];
        tri->chart_y = root_y[root];
        tri->chart_w = root_width[root];
        tri->chart_h = root_height[root];
        tri->texel_min_x = tri->chart_w - 1;
        tri->texel_min_y = tri->chart_h - 1;
        tri->texel_max_x = 0;
        tri->texel_max_y = 0;
        for (int corner = 0; corner < 3; ++corner) {
            double projected_u = positions[corner][0] * basis_u[root][0] +
                                 positions[corner][1] * basis_u[root][1] +
                                 positions[corner][2] * basis_u[root][2];
            double projected_v = positions[corner][0] * basis_v[root][0] +
                                 positions[corner][1] * basis_v[root][1] +
                                 positions[corner][2] * basis_v[root][2];
            tri->chart_uv[corner][0] = 0.5 + (projected_u - bounds[root][0]) * scale_u;
            tri->chart_uv[corner][1] = 0.5 + (projected_v - bounds[root][1]) * scale_v;
            int32_t tx = (int32_t)floor(tri->chart_uv[corner][0]);
            int32_t ty = (int32_t)floor(tri->chart_uv[corner][1]);
            if (tx < tri->texel_min_x)
                tri->texel_min_x = tx;
            if (ty < tri->texel_min_y)
                tri->texel_min_y = ty;
            if (tx > tri->texel_max_x)
                tri->texel_max_x = tx;
            if (ty > tri->texel_max_y)
                tri->texel_max_y = ty;
        }
        if (tri->texel_min_x < 0)
            tri->texel_min_x = 0;
        if (tri->texel_min_y < 0)
            tri->texel_min_y = 0;
        if (tri->texel_max_x >= tri->chart_w)
            tri->texel_max_x = tri->chart_w - 1;
        if (tri->texel_max_y >= tri->chart_h)
            tri->texel_max_y = tri->chart_h - 1;
        uint64_t texels = baker_chart_triangle_texel_count(tri);
        if (texels > UINT64_MAX / samples ||
            baker->total_sample_work > UINT64_MAX - texels * samples)
            goto cleanup;
        baker->total_sample_work += texels * samples;
    }
    result = baker->total_sample_work > 0;

cleanup:
    free(edges);
    free(rects);
    free(parents);
    free(root_width);
    free(root_height);
    free(root_x);
    free(root_y);
    free(skyline);
    free(basis_u);
    free(basis_v);
    free(bounds);
    return result;
}

/// @brief Release transient HDR/coverage storage after publication or failure.
static void baker_release_atlas_scratch(rt_lightbaker3d *baker) {
    if (!baker)
        return;
    free(baker->atlas_hdr);
    free(baker->atlas_coverage);
    baker->atlas_hdr = NULL;
    baker->atlas_coverage = NULL;
}

/// @brief End a bake failure without exposing a partial atlas or scratch state.
static int8_t baker_fail(rt_lightbaker3d *baker) {
    if (!baker)
        return 1;
    baker_release_ref(&baker->atlas);
    baker_release_atlas_scratch(baker);
    baker->failed = 1;
    baker->done = 1;
    baker->progress = 1.0;
    return 1;
}

/// @brief Verify that every captured source and UV target still matches the snapshot.
static int baker_validate_uv_targets(const rt_lightbaker3d *baker) {
    if (!baker)
        return 0;
    for (int32_t n = 0; n < baker->node_count; ++n) {
        const baker_node_entry *entry = &baker->nodes[n];
        double world_matrix[16];
        double albedo[3];
        double emissive[3];
        const rt_mesh3d *mesh =
            (const rt_mesh3d *)rt_g3d_checked_or_null(entry->mesh, RT_G3D_MESH3D_CLASS_ID);
        if (!mesh || !rt_scene_node3d_get_static(entry->node) ||
            rt_scene_node3d_get_mesh(entry->node) != entry->mesh ||
            rt_scene_node3d_get_material(entry->node) != entry->material ||
            !rt_scene_node3d_get_world_matrix_components(entry->node, world_matrix) ||
            mesh->geometry_revision != entry->geometry_revision)
            return 0;
        baker_read_material_colors(entry->material, albedo, emissive);
        for (int component = 0; component < 16; ++component)
            if (world_matrix[component] != entry->world_matrix[component])
                return 0;
        for (int channel = 0; channel < 3; ++channel)
            if (albedo[channel] != entry->albedo[channel] ||
                emissive[channel] != entry->emissive[channel])
                return 0;
    }
    for (int32_t i = 0; i < baker->tri_count; ++i) {
        const baker_tri *tri = &baker->tris[i];
        const rt_mesh3d *mesh;
        uint32_t vertex_count;
        if (tri->node_index < 0 || tri->node_index >= baker->node_count)
            return 0;
        mesh = (const rt_mesh3d *)baker->nodes[tri->node_index].mesh;
        vertex_count = rt_mesh3d_safe_vertex_count(mesh);
        for (int corner = 0; corner < 3; ++corner)
            if (tri->vertex_indices[corner] >= vertex_count)
                return 0;
    }
    return 1;
}

/// @brief Clone one node mesh, append seam-safe chart vertices, and rewrite captured indices.
/// @details A linear-time hash reuses `(source vertex, UV island)` pairs while keeping different
/// islands private, avoiding both shared-vertex UV overwrites and quadratic publication scans.
/// @param baker Baker containing chart metadata.
/// @param node_index Valid baked-node index.
/// @return Caller-owned cloned mesh, or NULL on validation/allocation failure.
static rt_mesh3d *baker_clone_node_lightmap_mesh(rt_lightbaker3d *baker, int32_t node_index) {
    baker_node_entry *entry;
    rt_mesh3d *source;
    rt_mesh3d *clone;
    uint32_t source_vertex_count;
    uint32_t destination_capacity;
    uint32_t destination_vertex_count;
    uint32_t *vertex_sources = NULL;
    uint64_t *vertex_keys = NULL;
    uint32_t *vertex_values = NULL;
    uint32_t hash_capacity = 16;
    double inv_dim;
    if (!baker || node_index < 0 || node_index >= baker->node_count)
        return NULL;
    entry = &baker->nodes[node_index];
    source = (rt_mesh3d *)entry->mesh;
    source_vertex_count = rt_mesh3d_safe_vertex_count(source);
    if (!source || entry->tri_count < 0 ||
        (uint64_t)source_vertex_count + (uint64_t)entry->tri_count * 3u > UINT32_MAX)
        return NULL;
    destination_capacity = source_vertex_count + (uint32_t)entry->tri_count * 3u;
    destination_vertex_count = source_vertex_count;
    while ((uint64_t)hash_capacity < (uint64_t)(uint32_t)entry->tri_count * 6u) {
        if (hash_capacity > UINT32_MAX / 2u)
            return NULL;
        hash_capacity *= 2u;
    }
    if (!rt_alloc_count_ok(destination_capacity, sizeof(vgfx3d_vertex_t)) ||
        (source->positions64 && !rt_alloc_count_ok(destination_capacity, 3u * sizeof(double))) ||
        (source->extra_influences &&
         !rt_alloc_count_ok(destination_capacity, sizeof(vgfx3d_extra_influences_t))) ||
        (source->morph_targets_ref &&
         !rt_alloc_count_ok(destination_capacity, sizeof(*vertex_sources))) ||
        !rt_alloc_count_ok(hash_capacity, sizeof(*vertex_keys)) ||
        !rt_alloc_count_ok(hash_capacity, sizeof(*vertex_values)))
        return NULL;
    clone = (rt_mesh3d *)rt_mesh3d_clone_for_lightmap(source);
    if (!clone)
        return NULL;
    if (destination_capacity > source_vertex_count) {
        vgfx3d_vertex_t *vertices = (vgfx3d_vertex_t *)realloc(
            clone->vertices, (size_t)destination_capacity * sizeof(*vertices));
        if (!vertices)
            goto fail;
        clone->vertices = vertices;
        if (clone->positions64) {
            double *positions = (double *)realloc(
                clone->positions64, (size_t)destination_capacity * 3u * sizeof(*positions));
            if (!positions)
                goto fail;
            clone->positions64 = positions;
        }
        if (clone->extra_influences) {
            vgfx3d_extra_influences_t *extra = (vgfx3d_extra_influences_t *)realloc(
                clone->extra_influences, (size_t)destination_capacity * sizeof(*extra));
            if (!extra)
                goto fail;
            clone->extra_influences = extra;
        }
    }
    if (source->morph_targets_ref) {
        vertex_sources = (uint32_t *)malloc((size_t)destination_capacity * sizeof(*vertex_sources));
        if (!vertex_sources)
            goto fail;
        for (uint32_t vertex = 0; vertex < source_vertex_count; ++vertex)
            vertex_sources[vertex] = vertex;
    }
    vertex_keys = (uint64_t *)malloc((size_t)hash_capacity * sizeof(*vertex_keys));
    vertex_values = (uint32_t *)malloc((size_t)hash_capacity * sizeof(*vertex_values));
    if (!vertex_keys || !vertex_values)
        goto fail;
    memset(vertex_keys, 0xFF, (size_t)hash_capacity * sizeof(*vertex_keys));
    inv_dim = 1.0 / (double)baker->atlas_dim;
    for (int32_t local_tri = 0; local_tri < entry->tri_count; ++local_tri) {
        baker_tri *tri = &baker->tris[entry->first_tri + local_tri];
        size_t first_index = (size_t)tri->tri_index * 3u;
        float uv[3][2];
        for (int corner = 0; corner < 3; ++corner) {
            uv[corner][0] = (float)((tri->chart_x + tri->chart_uv[corner][0]) * inv_dim);
            uv[corner][1] = (float)((tri->chart_y + tri->chart_uv[corner][1]) * inv_dim);
        }
        if (tri->node_index != node_index || first_index + 2u >= clone->index_count)
            goto fail;
        for (int corner = 0; corner < 3; ++corner) {
            uint32_t source_vertex = tri->vertex_indices[corner];
            uint64_t key = ((uint64_t)(uint32_t)tri->chart_root << 32) | source_vertex;
            uint64_t hash = key;
            hash ^= hash >> 33;
            hash *= UINT64_C(0xff51afd7ed558ccd);
            hash ^= hash >> 33;
            uint32_t slot = (uint32_t)hash & (hash_capacity - 1u);
            while (vertex_keys[slot] != UINT64_MAX && vertex_keys[slot] != key)
                slot = (slot + 1u) & (hash_capacity - 1u);
            if (source_vertex >= source_vertex_count)
                goto fail;
            uint32_t destination_vertex;
            if (vertex_keys[slot] == key) {
                destination_vertex = vertex_values[slot];
            } else {
                if (destination_vertex_count >= destination_capacity)
                    goto fail;
                destination_vertex = destination_vertex_count++;
                vertex_keys[slot] = key;
                vertex_values[slot] = destination_vertex;
                clone->vertices[destination_vertex] = source->vertices[source_vertex];
                clone->vertices[destination_vertex].uv1[0] = uv[corner][0];
                clone->vertices[destination_vertex].uv1[1] = uv[corner][1];
                if (clone->positions64)
                    memcpy(&clone->positions64[(size_t)destination_vertex * 3u],
                           &source->positions64[(size_t)source_vertex * 3u],
                           3u * sizeof(double));
                if (clone->extra_influences)
                    clone->extra_influences[destination_vertex] =
                        source->extra_influences[source_vertex];
                if (vertex_sources)
                    vertex_sources[destination_vertex] = source_vertex;
            }
            clone->indices[first_index + (size_t)corner] = destination_vertex;
        }
    }
    clone->vertex_count = destination_vertex_count;
    clone->vertex_capacity = destination_capacity;
    if (source->morph_targets_ref) {
        void *remapped = rt_morphtarget3d_clone_remapped(
            source->morph_targets_ref, vertex_sources, destination_vertex_count);
        if (!remapped)
            goto fail;
        rt_mesh3d_set_morph_targets(clone, remapped);
        baker_release_ref(&remapped);
    }
    free(vertex_sources);
    free(vertex_keys);
    free(vertex_values);
    rt_mesh3d_touch_geometry(clone);
    rt_mesh3d_refresh_bounds(clone);
    return clone;

fail:
    free(vertex_sources);
    free(vertex_keys);
    free(vertex_values);
    if (rt_obj_release_check0(clone))
        rt_obj_free(clone);
    return NULL;
}

/// @brief Transactionally publish seam-safe per-node lightmap mesh copies.
static int baker_publish_uvs(rt_lightbaker3d *baker) {
    rt_mesh3d **staged;
    if (!baker_validate_uv_targets(baker))
        return 0;
    staged = (rt_mesh3d **)calloc((size_t)baker->node_count, sizeof(*staged));
    if (!staged && baker->node_count > 0)
        return 0;
    for (int32_t node = 0; node < baker->node_count; ++node) {
        if (baker->nodes[node].tri_count <= 0)
            continue;
        staged[node] = baker_clone_node_lightmap_mesh(baker, node);
        if (!staged[node]) {
            for (int32_t release = 0; release < baker->node_count; ++release)
                baker_release_ref((void **)&staged[release]);
            free(staged);
            return 0;
        }
    }
    for (int32_t node = 0; node < baker->node_count; ++node) {
        baker_node_entry *entry = &baker->nodes[node];
        if (!staged[node])
            continue;
        rt_scene_node3d_set_mesh(entry->node, staged[node]);
        baker_release_ref(&entry->mesh);
        rt_obj_retain_maybe(staged[node]);
        entry->mesh = staged[node];
        entry->geometry_revision = staged[node]->geometry_revision;
        entry->uv1_written = 1;
        baker_release_ref((void **)&staged[node]);
    }
    free(staged);
    return 1;
}

/// @brief Encode one HDR radiance channel using the atlas's two-times headroom.
static uint32_t baker_encode_atlas_channel(float value) {
    if (!isfinite(value) || value <= 0.0f)
        return 0;
    if (value >= 2.0f)
        return 255;
    return (uint32_t)(value * 127.5f + 0.5f);
}

/// @brief Run one deterministic bake slice after transactional setup/preflight.
/// @details Each call processes a bounded number of radiance samples and updates progress. Source
///   UV1 data remains untouched until all sampling and atlas allocation succeed.
///   Terminal gather/BVH/capacity/allocation/source-change failure completes
///   without an atlas. Success publishes all UVs, dilates one texel ring, and
///   installs an 8-bit atlas with two-times radiance headroom.
/// @param obj LightBaker3D receiver.
/// @return 1 when the bake is complete, 0 when more steps remain.
int8_t rt_lightbaker3d_bake_step(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.BakeStep: invalid baker");
    float *hdr;
    uint8_t *coverage;
    if (!baker)
        return 1;
    if (baker->done)
        return 1;
    if ((!baker->gathered && !baker_gather_scene(baker)) || !baker_bvh_build(baker) ||
        !baker_build_light_index(baker))
        return baker_fail(baker);
    if (baker->tri_count == 0) {
        baker->done = 1;
        baker->progress = 1.0;
        return 1;
    }
    if (!baker->atlas_hdr || !baker->atlas_coverage) {
        if (baker->atlas_hdr || baker->atlas_coverage || baker->next_tri != 0 ||
            !baker_preflight_charts(baker))
            return baker_fail(baker);
        hdr = (float *)calloc((size_t)baker->atlas_dim * baker->atlas_dim * 3, sizeof(*hdr));
        coverage =
            (uint8_t *)calloc((size_t)baker->atlas_dim * baker->atlas_dim, sizeof(*coverage));
        if (!hdr || !coverage) {
            free(hdr);
            free(coverage);
            return baker_fail(baker);
        }
        baker->atlas_hdr = hdr;
        baker->atlas_coverage = coverage;
    }
    if (!baker_validate_uv_targets(baker))
        return baker_fail(baker);

    int32_t budget = BAKER3D_SAMPLE_WORK_PER_STEP;
    double sample_results[BAKER3D_SAMPLE_WORK_PER_STEP][3];
    while (baker->next_tri < baker->tri_count && budget > 0) {
        baker_tri *tri = &baker->tris[baker->next_tri];
        int32_t cx = tri->chart_x;
        int32_t cy = tri->chart_y;
        int32_t texel_width = tri->texel_max_x - tri->texel_min_x + 1;
        int32_t texel_height = tri->texel_max_y - tri->texel_min_y + 1;
        int32_t texel_count = texel_width * texel_height;
        int64_t samples = baker->samples < 1 ? 1 : baker->samples;
        if (baker->next_texel >= texel_count) {
            baker->next_tri++;
            baker->next_texel = 0;
            continue;
        }
        int32_t tx = tri->texel_min_x + baker->next_texel % texel_width;
        int32_t ty = tri->texel_min_y + baker->next_texel / texel_width;
        double u;
        double v;
        if (!baker_chart_texel_barycentric(tri, tx, ty, &u, &v)) {
            baker->next_texel++;
            continue;
        }
        double point[3] = {
            tri->p0[0] + (tri->p1[0] - tri->p0[0]) * u + (tri->p2[0] - tri->p0[0]) * v,
            tri->p0[1] + (tri->p1[1] - tri->p0[1]) * u + (tri->p2[1] - tri->p0[1]) * v,
            tri->p0[2] + (tri->p1[2] - tri->p0[2]) * u + (tri->p2[2] - tri->p0[2]) * v};
        if (baker->next_sample == 0) {
            baker->sample_acc[0] = baker->sample_acc[1] = baker->sample_acc[2] = 0.0;
        }
        int64_t remaining = samples - baker->next_sample;
        int32_t batch_count = remaining < budget ? (int32_t)remaining : budget;
        baker_evaluate_sample_batch(baker,
                                    tri,
                                    baker->next_tri,
                                    tx,
                                    ty,
                                    point,
                                    baker->next_sample,
                                    batch_count,
                                    sample_results);
        for (int32_t sample = 0; sample < batch_count; ++sample) {
            baker->sample_acc[0] += sample_results[sample][0];
            baker->sample_acc[1] += sample_results[sample][1];
            baker->sample_acc[2] += sample_results[sample][2];
        }
        baker->next_sample += batch_count;
        baker->completed_sample_work += (uint64_t)batch_count;
        budget -= batch_count;
        if (baker->next_sample == samples) {
            size_t at = ((size_t)(cy + ty) * baker->atlas_dim + (cx + tx));
            baker->atlas_hdr[at * 3 + 0] = (float)(baker->sample_acc[0] / (double)samples);
            baker->atlas_hdr[at * 3 + 1] = (float)(baker->sample_acc[1] / (double)samples);
            baker->atlas_hdr[at * 3 + 2] = (float)(baker->sample_acc[2] / (double)samples);
            baker->atlas_coverage[at] = 1;
            baker->next_sample = 0;
            baker->next_texel++;
        }
    }
    while (baker->next_tri < baker->tri_count) {
        baker_tri *tri = &baker->tris[baker->next_tri];
        int32_t texel_count =
            (tri->texel_max_x - tri->texel_min_x + 1) * (tri->texel_max_y - tri->texel_min_y + 1);
        if (baker->next_texel < texel_count)
            break;
        baker->next_tri++;
        baker->next_texel = 0;
    }
    baker->progress = baker->total_sample_work > 0
                          ? (double)baker->completed_sample_work / (double)baker->total_sample_work
                          : 1.0;
    if (baker->next_tri < baker->tri_count)
        return 0;

    /* Finish: dilate uncovered texels from covered neighbors and publish the atlas. */
    rt_pixels_impl *atlas = (rt_pixels_impl *)rt_pixels_new(baker->atlas_dim, baker->atlas_dim);
    if (!atlas)
        return baker_fail(baker);
    if (!atlas->data) {
        if (rt_obj_release_check0(atlas))
            rt_obj_free(atlas);
        return baker_fail(baker);
    }
    for (int32_t y = 0; y < baker->atlas_dim; ++y) {
        for (int32_t x = 0; x < baker->atlas_dim; ++x) {
            size_t at = (size_t)y * baker->atlas_dim + x;
            float r = baker->atlas_hdr[at * 3 + 0];
            float g = baker->atlas_hdr[at * 3 + 1];
            float b = baker->atlas_hdr[at * 3 + 2];
            if (!baker->atlas_coverage[at]) {
                float best[3] = {0.0f, 0.0f, 0.0f};
                int found = 0;
                for (int dy = -1; dy <= 1 && !found; ++dy) {
                    for (int dx = -1; dx <= 1 && !found; ++dx) {
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= baker->atlas_dim || ny >= baker->atlas_dim)
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
            atlas->data[at] = (baker_encode_atlas_channel(r) << 24) |
                              (baker_encode_atlas_channel(g) << 16) |
                              (baker_encode_atlas_channel(b) << 8) | 0xFFu;
        }
    }
    pixels_touch(atlas);
    atlas->alpha_scan_generation = atlas->generation;
    atlas->alpha_scan_classification = RT_PIXELS_ALPHA_OPAQUE;
    atlas->alpha_scan_valid = 1;
    if (!baker_publish_uvs(baker)) {
        if (rt_obj_release_check0(atlas))
            rt_obj_free(atlas);
        return baker_fail(baker);
    }
    baker_release_ref(&baker->atlas);
    baker->atlas = atlas;
    baker_release_atlas_scratch(baker);
    baker->done = 1;
    baker->progress = 1.0;
    return 1;
}

/// @brief Install the baked atlas on every baked node via material instances.
/// @details Stages every eligible material instance before mutating any scene
///   node, then installs all instances as one failure-atomic, idempotent apply.
///   Incomplete, failed, repeated, or atlas-less calls are ignored.
/// @param obj LightBaker3D receiver.
void rt_lightbaker3d_apply(void *obj) {
    rt_lightbaker3d *baker = lightbaker3d_checked(obj, "LightBaker3D.Apply: invalid baker");
    void **instances;
    if (!baker || !baker->done || baker->failed || baker->applied || !baker->atlas ||
        !baker_validate_uv_targets(baker))
        return;
    instances = (void **)calloc((size_t)baker->node_count, sizeof(*instances));
    if (!instances && baker->node_count > 0)
        return;
    for (int32_t n = 0; n < baker->node_count; ++n) {
        baker_node_entry *entry = &baker->nodes[n];
        if (!entry->uv1_written)
            continue;
        instances[n] =
            entry->material ? rt_material3d_make_instance(entry->material) : rt_material3d_new();
        if (!instances[n]) {
            for (int32_t staged = 0; staged < baker->node_count; ++staged)
                baker_release_ref(&instances[staged]);
            free(instances);
            return;
        }
        rt_material3d_set_lightmap(instances[n], baker->atlas);
    }
    for (int32_t n = 0; n < baker->node_count; ++n) {
        if (!instances[n])
            continue;
        rt_scene_node3d_set_material(baker->nodes[n].node, instances[n]);
        baker_release_ref(&instances[n]);
    }
    free(instances);
    baker->applied = 1;
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
    int32_t counts[3];
    size_t probes;
    float *sh;
    uint8_t *valid;
    rt_lightprobegrid3d *grid;
    if (!rt_g3d_is_vec3(min_v) || !rt_g3d_is_vec3(max_v)) {
        rt_trap("LightProbeGrid3D.New: bounds must be Vec3");
        return NULL;
    }
    if (!baker_read_vec3(min_v, &min_b[0], &min_b[1], &min_b[2]) ||
        !baker_read_vec3(max_v, &max_b[0], &max_b[1], &max_b[2])) {
        rt_trap("LightProbeGrid3D.New: bounds must be finite and ordered");
        return NULL;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (max_b[axis] < min_b[axis]) {
            rt_trap("LightProbeGrid3D.New: bounds must be finite and ordered");
            return NULL;
        }
    }
    if (!isfinite(spacing) || spacing <= 0.01)
        spacing = 1.0;
    for (int axis = 0; axis < 3; ++axis) {
        double extent = max_b[axis] - min_b[axis];
        double intervals = extent / spacing;
        if (!isfinite(intervals) || intervals >= 63.0)
            counts[axis] = 64;
        else {
            counts[axis] = (int32_t)ceil(intervals) + 1;
            if (counts[axis] < 2)
                counts[axis] = 2;
        }
    }
    probes = (size_t)counts[0] * (size_t)counts[1] * (size_t)counts[2];
    sh = (float *)calloc(probes * 27u, sizeof(*sh));
    valid = (uint8_t *)calloc(probes, sizeof(*valid));
    if (!sh || !valid) {
        free(sh);
        free(valid);
        rt_trap("LightProbeGrid3D.New: coefficient allocation failed");
        return NULL;
    }
    grid = (rt_lightprobegrid3d *)rt_obj_new_i64(RT_G3D_LIGHTPROBEGRID3D_CLASS_ID,
                                                 (int64_t)sizeof(rt_lightprobegrid3d));
    if (!grid) {
        free(sh);
        free(valid);
        rt_trap("LightProbeGrid3D.New: allocation failed");
        return NULL;
    }
    memset(grid, 0, sizeof(*grid));
    rt_obj_set_finalizer(grid, lightprobegrid3d_finalize);
    memcpy(grid->min_b, min_b, sizeof(grid->min_b));
    grid->spacing = spacing;
    grid->nx = counts[0];
    grid->ny = counts[1];
    grid->nz = counts[2];
    grid->sh = sh;
    grid->valid = valid;
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

/// @brief Add a double contribution to a finite float SH coefficient safely.
static float probe_accumulate_coefficient(float current, double contribution) {
    double sum = (double)current + contribution;
    if (isnan(sum))
        return current;
    if (!isfinite(sum))
        return sum < 0.0 ? -FLT_MAX : FLT_MAX;
    if (sum > FLT_MAX)
        return FLT_MAX;
    if (sum < -FLT_MAX)
        return -FLT_MAX;
    return (float)sum;
}

typedef struct probe_bake_task {
    rt_lightprobegrid3d *grid;
    const rt_lightbaker3d *baker;
    size_t first_probe;
    size_t probe_count;
    int64_t direction_samples;
} probe_bake_task;

/// @brief Bake a disjoint deterministic range of probe coefficients.
static void probe_bake_task_run(void *argument) {
    probe_bake_task *task = (probe_bake_task *)argument;
    if (!task || !task->grid || !task->baker || task->direction_samples <= 0)
        return;
    rt_lightprobegrid3d *grid = task->grid;
    const rt_lightbaker3d *baker = task->baker;
    int64_t dir_samples = task->direction_samples;
    size_t end = task->first_probe + task->probe_count;
    for (size_t p = task->first_probe; p < end; ++p) {
        int32_t px = (int32_t)(p % (size_t)grid->nx);
        int32_t py = (int32_t)((p / (size_t)grid->nx) % (size_t)grid->ny);
        int32_t pz = (int32_t)(p / ((size_t)grid->nx * (size_t)grid->ny));
        double point[3] = {grid->min_b[0] + px * grid->spacing,
                           grid->min_b[1] + py * grid->spacing,
                           grid->min_b[2] + pz * grid->spacing};
        uint32_t rng = (uint32_t)(p * 40507u + 7u);
        float *sh = grid->sh + p * 27u;
        int inside_hits = 0;
        memset(sh, 0, 27u * sizeof(*sh));
        for (int64_t sample = 0; sample < dir_samples; ++sample) {
            double z = 1.0 - 2.0 * baker_lcg_unit(&rng);
            double phi = 2.0 * 3.14159265358979323846 * baker_lcg_unit(&rng);
            double radial = sqrt(fmax(0.0, 1.0 - z * z));
            double direction[3] = {radial * cos(phi), z, radial * sin(phi)};
            double distance;
            int32_t hit = baker_trace(baker, point, direction, DBL_MAX, &distance);
            double rgb[3];
            if (hit < 0) {
                rgb[0] = baker->sky_color[0];
                rgb[1] = baker->sky_color[1];
                rgb[2] = baker->sky_color[2];
            } else {
                const baker_tri *tri = &baker->tris[hit];
                double facing = tri->normal[0] * direction[0] + tri->normal[1] * direction[1] +
                                tri->normal[2] * direction[2];
                if (facing > 0.0 && distance < grid->spacing * 0.5)
                    inside_hits++;
                double hit_point[3] = {point[0] + direction[0] * distance,
                                       point[1] + direction[1] * distance,
                                       point[2] + direction[2] * distance};
                double hit_normal[3] = {tri->normal[0], tri->normal[1], tri->normal[2]};
                if (facing > 0.0) {
                    hit_normal[0] = -hit_normal[0];
                    hit_normal[1] = -hit_normal[1];
                    hit_normal[2] = -hit_normal[2];
                }
                baker_radiance(baker, hit_point, hit_normal, &rng, (int32_t)baker->bounces, 1, rgb);
                rgb[0] = rgb[0] * tri->albedo[0] + tri->emissive[0];
                rgb[1] = rgb[1] * tri->albedo[1] + tri->emissive[1];
                rgb[2] = rgb[2] * tri->albedo[2] + tri->emissive[2];
            }
            double basis[9];
            probe_sh_basis(direction, basis);
            double weight = 4.0 * 3.14159265358979323846 / (double)dir_samples;
            for (int coefficient = 0; coefficient < 9; ++coefficient) {
                sh[coefficient * 3 + 0] = probe_accumulate_coefficient(
                    sh[coefficient * 3 + 0], rgb[0] * basis[coefficient] * weight);
                sh[coefficient * 3 + 1] = probe_accumulate_coefficient(
                    sh[coefficient * 3 + 1], rgb[1] * basis[coefficient] * weight);
                sh[coefficient * 3 + 2] = probe_accumulate_coefficient(
                    sh[coefficient * 3 + 2], rgb[2] * basis[coefficient] * weight);
            }
        }
        grid->valid[p] = inside_hits * 4 < dir_samples ? 1 : 0;
    }
}

/// @brief Bake the probe grid against a baker's gathered scene + lights.
/// @details Reuses the baker's BVH and radiance estimator so probes and
///   lightmaps agree; the baker must be constructed over the same scene (its
///   gather runs here when the baker has not baked yet). Directions are sampled
///   uniformly on the sphere with deterministic seeds. Probes likely inside
///   geometry are marked invalid and breadth-first in-filled from the nearest
///   valid axis-connected region.
/// @param obj LightProbeGrid3D receiver whose coefficient arrays are overwritten.
/// @param baker_obj LightBaker3D providing scene BVH, copied lights, sky, samples,
///   and bounce depth.
void rt_lightprobegrid3d_bake(void *obj, void *baker_obj) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Bake: invalid grid");
    rt_lightbaker3d *baker =
        lightbaker3d_checked(baker_obj, "LightProbeGrid3D.Bake: invalid baker");
    size_t probes;
    size_t *queue;
    if (!grid || !baker || !grid->sh || !grid->valid)
        return;
    if ((!baker->gathered && !baker_gather_scene(baker)) || !baker_bvh_build(baker) ||
        !baker_build_light_index(baker))
        return;
    probes = (size_t)grid->nx * (size_t)grid->ny * (size_t)grid->nz;
    queue = (size_t *)malloc(probes * sizeof(*queue));
    if (!queue)
        return;
    grid->baked = 0;
    int64_t dir_samples = baker->samples < 8 ? 8 : (baker->samples > 256 ? 256 : baker->samples);
    void *pool = probes >= 8 ? baker_worker_pool(baker) : NULL;
    int32_t task_count = pool ? baker->worker_count : 1;
    if ((size_t)task_count > probes)
        task_count = (int32_t)probes;
    probe_bake_task tasks[BAKER3D_MAX_PARALLEL_TASKS];
    size_t base_count = probes / (size_t)task_count;
    size_t extra = probes % (size_t)task_count;
    size_t first_probe = 0;
    for (int32_t task_index = 0; task_index < task_count; ++task_index) {
        probe_bake_task *task = &tasks[task_index];
        task->grid = grid;
        task->baker = baker;
        task->first_probe = first_probe;
        task->probe_count = base_count + ((size_t)task_index < extra ? 1u : 0u);
        task->direction_samples = dir_samples;
        first_probe += task->probe_count;
        if (!pool || !rt_threadpool_submit_fn(pool, probe_bake_task_run, task))
            probe_bake_task_run(task);
    }
    if (pool)
        rt_threadpool_wait(pool);
    {
        static const int32_t offsets[6][3] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        size_t head = 0;
        size_t tail = 0;
        for (size_t p = 0; p < probes; ++p)
            if (grid->valid[p])
                queue[tail++] = p;
        while (head < tail) {
            size_t p = queue[head++];
            int32_t px = (int32_t)(p % (size_t)grid->nx);
            int32_t py = (int32_t)((p / (size_t)grid->nx) % (size_t)grid->ny);
            int32_t pz = (int32_t)(p / ((size_t)grid->nx * (size_t)grid->ny));
            for (int offset = 0; offset < 6; ++offset) {
                int32_t qx = px + offsets[offset][0];
                int32_t qy = py + offsets[offset][1];
                int32_t qz = pz + offsets[offset][2];
                size_t q;
                if (qx < 0 || qy < 0 || qz < 0 || qx >= grid->nx || qy >= grid->ny ||
                    qz >= grid->nz)
                    continue;
                q = ((size_t)qz * (size_t)grid->ny + (size_t)qy) * (size_t)grid->nx + (size_t)qx;
                if (grid->valid[q])
                    continue;
                memcpy(grid->sh + q * 27u, grid->sh + p * 27u, 27u * sizeof(float));
                grid->valid[q] = 1;
                queue[tail++] = q;
            }
        }
    }
    free(queue);
    grid->baked = 1;
}

/// @brief Resolve one clamped grid coordinate without unsafe integer narrowing.
static void probe_sample_axis(double position,
                              double minimum,
                              double spacing,
                              int32_t count,
                              int32_t *cell,
                              double *fraction) {
    double coordinate = (position - minimum) / spacing;
    if (!isfinite(coordinate)) {
        if (position <= minimum) {
            *cell = 0;
            *fraction = 0.0;
        } else {
            *cell = count - 2;
            *fraction = 1.0;
        }
    } else if (coordinate <= 0.0) {
        *cell = 0;
        *fraction = 0.0;
    } else if (coordinate >= (double)(count - 1)) {
        *cell = count - 2;
        *fraction = 1.0;
    } else {
        *cell = (int32_t)floor(coordinate);
        *fraction = coordinate - (double)*cell;
    }
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
    if (!grid || !grid->baked || !grid->sh || !grid->valid ||
        !baker_read_vec3(position, &pos[0], &pos[1], &pos[2]))
        return rt_vec3_new(0.0, 0.0, 0.0);
    if (normal)
        (void)baker_read_vec3(normal, &nrm[0], &nrm[1], &nrm[2]);
    double nlen = hypot(hypot(nrm[0], nrm[1]), nrm[2]);
    if (isfinite(nlen) && nlen > 1e-9) {
        nrm[0] /= nlen;
        nrm[1] /= nlen;
        nrm[2] /= nlen;
    } else {
        nrm[0] = 0.0;
        nrm[1] = 1.0;
        nrm[2] = 0.0;
    }
    int32_t x0, y0, z0;
    double fx, fy, fz;
    probe_sample_axis(pos[0], grid->min_b[0], grid->spacing, grid->nx, &x0, &fx);
    probe_sample_axis(pos[1], grid->min_b[1], grid->spacing, grid->ny, &y0, &fy);
    probe_sample_axis(pos[2], grid->min_b[2], grid->spacing, grid->nz, &z0, &fz);
    double sh[27];
    double valid_weight = 0.0;
    memset(sh, 0, sizeof(sh));
    for (int corner = 0; corner < 8; ++corner) {
        int32_t cx = x0 + (corner & 1);
        int32_t cy = y0 + ((corner >> 1) & 1);
        int32_t cz = z0 + ((corner >> 2) & 1);
        double w = ((corner & 1) ? fx : 1.0 - fx) * (((corner >> 1) & 1) ? fy : 1.0 - fy) *
                   (((corner >> 2) & 1) ? fz : 1.0 - fz);
        size_t probe = ((size_t)cz * (size_t)grid->ny + (size_t)cy) * (size_t)grid->nx + (size_t)cx;
        const float *psh;
        if (!grid->valid[probe] || w <= 0.0)
            continue;
        psh = grid->sh + probe * 27u;
        for (int c = 0; c < 27; ++c)
            sh[c] += psh[c] * w;
        valid_weight += w;
    }
    if (!isfinite(valid_weight) || valid_weight <= 0.0)
        return rt_vec3_new(0.0, 0.0, 0.0);
    for (int coefficient = 0; coefficient < 27; ++coefficient)
        sh[coefficient] /= valid_weight;
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
 * Serialization (.vlpg — versioned little-endian IEEE probe grids)
 *=========================================================================*/

#define VLPG_MAGIC "VLPG0001"

/// @brief Store one unsigned 32-bit value in canonical little-endian order.
static void vlpg_store_u32_le(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
    out[2] = (uint8_t)(value >> 16u);
    out[3] = (uint8_t)(value >> 24u);
}

/// @brief Decode one canonical little-endian unsigned 32-bit value.
static uint32_t vlpg_load_u32_le(const uint8_t in[4]) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8u) | ((uint32_t)in[2] << 16u) |
           ((uint32_t)in[3] << 24u);
}

/// @brief Store one unsigned 64-bit value in canonical little-endian order.
static void vlpg_store_u64_le(uint8_t out[8], uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
        out[byte] = (uint8_t)(value >> (byte * 8u));
}

/// @brief Decode one canonical little-endian unsigned 64-bit value.
static uint64_t vlpg_load_u64_le(const uint8_t in[8]) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte)
        value |= (uint64_t)in[byte] << (byte * 8u);
    return value;
}

/// @brief Return whether the host exposes the IEEE layouts required by VLPG.
static int vlpg_ieee_layout_supported(void) {
    return sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 &&
           FLT_MAX_EXP == 128 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024;
}

/// @brief Write one little-endian unsigned 32-bit value.
static int vlpg_write_u32(FILE *file, uint32_t value) {
    uint8_t encoded[4];
    vlpg_store_u32_le(encoded, value);
    return fwrite(encoded, 1, sizeof(encoded), file) == sizeof(encoded);
}

/// @brief Read one little-endian unsigned 32-bit value.
static int vlpg_read_u32(FILE *file, uint32_t *value) {
    uint8_t encoded[4];
    if (!value || fread(encoded, 1, sizeof(encoded), file) != sizeof(encoded))
        return 0;
    *value = vlpg_load_u32_le(encoded);
    return 1;
}

/// @brief Write one little-endian IEEE binary64 value.
static int vlpg_write_f64(FILE *file, double value) {
    uint64_t bits;
    uint8_t encoded[8];
    memcpy(&bits, &value, sizeof(bits));
    vlpg_store_u64_le(encoded, bits);
    return fwrite(encoded, 1, sizeof(encoded), file) == sizeof(encoded);
}

/// @brief Read one little-endian IEEE binary64 value.
static int vlpg_read_f64(FILE *file, double *value) {
    uint8_t encoded[8];
    uint64_t bits;
    if (!value || fread(encoded, 1, sizeof(encoded), file) != sizeof(encoded))
        return 0;
    bits = vlpg_load_u64_le(encoded);
    memcpy(value, &bits, sizeof(bits));
    return 1;
}

/// @brief Encode and write a float array in bounded chunks.
static int vlpg_write_f32_array(FILE *file, const float *values, size_t count) {
    uint8_t encoded[4096];
    size_t offset = 0;
    if (!file || (!values && count > 0))
        return 0;
    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > sizeof(encoded) / 4u)
            chunk = sizeof(encoded) / 4u;
        for (size_t i = 0; i < chunk; ++i) {
            uint32_t bits;
            memcpy(&bits, &values[offset + i], sizeof(bits));
            vlpg_store_u32_le(encoded + i * 4u, bits);
        }
        if (fwrite(encoded, 4u, chunk, file) != chunk)
            return 0;
        offset += chunk;
    }
    return 1;
}

/// @brief Read and validate a canonical finite float array in bounded chunks.
static int vlpg_read_f32_array(FILE *file, float *values, size_t count) {
    uint8_t encoded[4096];
    size_t offset = 0;
    if (!file || (!values && count > 0))
        return 0;
    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > sizeof(encoded) / 4u)
            chunk = sizeof(encoded) / 4u;
        if (fread(encoded, 4u, chunk, file) != chunk)
            return 0;
        for (size_t i = 0; i < chunk; ++i) {
            uint32_t bits = vlpg_load_u32_le(encoded + i * 4u);
            memcpy(&values[offset + i], &bits, sizeof(bits));
            if (!isfinite(values[offset + i]))
                return 0;
        }
        offset += chunk;
    }
    return 1;
}

/// @brief Validate a complete in-memory probe payload before persistence.
static int vlpg_grid_payload_valid(const rt_lightprobegrid3d *grid) {
    size_t probes;
    if (!grid || !grid->baked || !grid->sh || !grid->valid || !isfinite(grid->spacing) ||
        grid->spacing <= 0.0 || grid->nx < 2 || grid->ny < 2 || grid->nz < 2 || grid->nx > 64 ||
        grid->ny > 64 || grid->nz > 64)
        return 0;
    for (int axis = 0; axis < 3; ++axis)
        if (!isfinite(grid->min_b[axis]))
            return 0;
    probes = (size_t)grid->nx * (size_t)grid->ny * (size_t)grid->nz;
    for (size_t probe = 0; probe < probes; ++probe)
        if (grid->valid[probe] > 1)
            return 0;
    for (size_t coefficient = 0; coefficient < probes * 27u; ++coefficient)
        if (!isfinite(grid->sh[coefficient]))
            return 0;
    return 1;
}

/// @brief Save a baked probe grid to the versioned VLPG binary layout.
/// @details Writes magic, little-endian IEEE metadata, canonical validity, and
///   probe-major float coefficients to a same-directory temporary file, then
///   atomically replaces the destination only after a complete close.
/// @param obj Baked LightProbeGrid3D receiver.
/// @param path Runtime string naming the output filesystem path.
/// @return Nonzero only when every write and file open succeeds.
int8_t rt_lightprobegrid3d_save(void *obj, rt_string path) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Save: invalid grid");
    const char *cpath;
    char *tmp_path = NULL;
    FILE *f;
    size_t probes;
    int ok;
    if (!grid || !rt_string_is_handle(path) || !vlpg_ieee_layout_supported() ||
        !vlpg_grid_payload_valid(grid))
        return 0;
    cpath = rt_string_cstr(path);
    if (!cpath || cpath[0] == '\0')
        return 0;
    f = rt_file_stdio_open_temp_for_replace_utf8(cpath, &tmp_path);
    if (!f)
        return 0;
    probes = (size_t)grid->nx * (size_t)grid->ny * (size_t)grid->nz;
    ok = fwrite(VLPG_MAGIC, 1, 8, f) == 8;
    for (int axis = 0; axis < 3 && ok; ++axis)
        ok = vlpg_write_f64(f, grid->min_b[axis]);
    ok = ok && vlpg_write_f64(f, grid->spacing) && vlpg_write_u32(f, (uint32_t)grid->nx) &&
         vlpg_write_u32(f, (uint32_t)grid->ny) && vlpg_write_u32(f, (uint32_t)grid->nz) &&
         fwrite(grid->valid, 1, probes, f) == probes &&
         vlpg_write_f32_array(f, grid->sh, probes * 27u);
    if (!rt_file_stdio_flush_sync_close(f))
        ok = 0;
    if (ok)
        ok = rt_file_stdio_replace_utf8(tmp_path, cpath);
    if (!ok)
        (void)rt_file_stdio_unlink_utf8(tmp_path);
    free(tmp_path);
    return ok ? 1 : 0;
}

/// @brief Load a versioned VLPG binary into an existing probe grid.
/// @details Decodes canonical little-endian fields, validates every byte of the
///   staged payload and exact EOF, closes the input, then commits replacement
///   storage so every malformed or I/O-failed load leaves the grid unchanged.
/// @param obj LightProbeGrid3D receiver to replace on success.
/// @param path Runtime string naming the input filesystem path.
/// @return Nonzero on a complete valid load; zero for invalid input, I/O,
///   validation, or allocation failure.
int8_t rt_lightprobegrid3d_load(void *obj, rt_string path) {
    rt_lightprobegrid3d *grid =
        lightprobegrid3d_checked(obj, "LightProbeGrid3D.Load: invalid grid");
    const char *cpath;
    FILE *f;
    char magic[8] = {0};
    double min_b[3] = {0.0, 0.0, 0.0};
    double spacing = 0.0;
    uint32_t dimensions[3] = {0, 0, 0};
    uint8_t *valid = NULL;
    float *sh = NULL;
    size_t probes = 0;
    int ok;
    if (!grid || !rt_string_is_handle(path) || !vlpg_ieee_layout_supported())
        return 0;
    cpath = rt_string_cstr(path);
    if (!cpath || cpath[0] == '\0')
        return 0;
    f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f)
        return 0;
    ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, VLPG_MAGIC, 8) == 0;
    for (int axis = 0; axis < 3 && ok; ++axis)
        ok = vlpg_read_f64(f, &min_b[axis]);
    ok = ok && vlpg_read_f64(f, &spacing) && vlpg_read_u32(f, &dimensions[0]) &&
         vlpg_read_u32(f, &dimensions[1]) && vlpg_read_u32(f, &dimensions[2]);
    if (ok) {
        for (int axis = 0; axis < 3; ++axis)
            if (!isfinite(min_b[axis]) || dimensions[axis] < 2 || dimensions[axis] > 64)
                ok = 0;
        if (!isfinite(spacing) || spacing <= 0.0)
            ok = 0;
    }
    if (ok) {
        probes = (size_t)dimensions[0] * (size_t)dimensions[1] * (size_t)dimensions[2];
        valid = (uint8_t *)malloc(probes);
        sh = (float *)malloc(probes * 27u * sizeof(*sh));
        if (!valid || !sh || fread(valid, 1, probes, f) != probes ||
            !vlpg_read_f32_array(f, sh, probes * 27u)) {
            ok = 0;
        }
    }
    if (ok) {
        for (size_t probe = 0; probe < probes; ++probe)
            if (valid[probe] > 1) {
                ok = 0;
                break;
            }
    }
    if (ok) {
        int trailing = fgetc(f);
        if (trailing != EOF || ferror(f))
            ok = 0;
    }
    if (fclose(f) != 0)
        ok = 0;
    if (ok) {
        free(grid->valid);
        free(grid->sh);
        grid->valid = valid;
        grid->sh = sh;
        memcpy(grid->min_b, min_b, sizeof(grid->min_b));
        grid->spacing = spacing;
        grid->nx = (int32_t)dimensions[0];
        grid->ny = (int32_t)dimensions[1];
        grid->nz = (int32_t)dimensions[2];
        grid->baked = 1;
    } else {
        free(valid);
        free(sh);
    }
    return ok ? 1 : 0;
}

#else
typedef int rt_lightbaker3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
