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
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_pixels_internal.h"
#include "rt_scene3d.h"
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
    int32_t node_index;         /* source baked-node index */
    int32_t tri_index;          /* triangle index within the source mesh */
    uint32_t vertex_indices[3]; /* captured UV publication targets */
    int32_t chart_x, chart_y;
    int32_t chart_w, chart_h;
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
    int32_t type; /* 0 directional, 1 point, 3 spot (ambient skipped) */
    double direction[3];
    double position[3];
    double color[3];
    double intensity;
    double attenuation;
    double inner_cos;
    double outer_cos;
    double range;
    int32_t decay_type;
    int8_t casts_shadows;
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
/// @return Nonzero when the forward interval intersects before @p t_max.
static int baker_ray_aabb(const double origin[3],
                          const double dir[3],
                          const double min_b[3],
                          const double max_b[3],
                          double t_max) {
    double t0 = 0.0, t1 = t_max;
    for (int a = 0; a < 3; ++a) {
        double near_t;
        double far_t;
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
    int32_t stack[64];
    int32_t stack_count = 0;
    stack[stack_count++] = 0;
    int32_t best = -1;
    double best_t = t_max;
    while (stack_count > 0) {
        const baker_bvh_node *node = &baker->bvh_nodes[stack[--stack_count]];
        if (!baker_ray_aabb(origin, dir, node->min_b, node->max_b, best_t))
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
    for (int32_t li = 0; li < baker->light_count; ++li) {
        const baker_light *light = &baker->lights[li];
        double to_light[3];
        double dist = DBL_MAX;
        double atten = 1.0;
        if (light->type == 0) {
            to_light[0] = -light->direction[0];
            to_light[1] = -light->direction[1];
            to_light[2] = -light->direction[2];
        } else {
            to_light[0] = light->position[0] - point[0];
            to_light[1] = light->position[1] - point[1];
            to_light[2] = light->position[2] - point[2];
            dist = hypot(hypot(to_light[0], to_light[1]), to_light[2]);
            if (!isfinite(dist) || dist < 1e-9)
                continue;
            to_light[0] /= dist;
            to_light[1] /= dist;
            to_light[2] /= dist;
            atten = baker_light_distance_decay(light, dist) *
                    baker_light_range_fade(dist, light->range);
            if (atten <= 0.0)
                continue;
            if (light->type == 3) {
                double spot_dot = to_light[0] * -light->direction[0] +
                                  to_light[1] * -light->direction[1] +
                                  to_light[2] * -light->direction[2];
                if (spot_dot < light->outer_cos)
                    continue;
                if (spot_dot < light->inner_cos) {
                    double cone_range = light->inner_cos - light->outer_cos;
                    double t;
                    if (cone_range <= 1e-12)
                        continue;
                    t = (spot_dot - light->outer_cos) / cone_range;
                    atten *= t * t * (3.0 - 2.0 * t);
                }
            }
        }
        double ndl = normal[0] * to_light[0] + normal[1] * to_light[1] + normal[2] * to_light[2];
        if (ndl <= 0.0)
            continue;
        double origin[3] = {point[0] + normal[0] * BAKER3D_EPS * 4,
                            point[1] + normal[1] * BAKER3D_EPS * 4,
                            point[2] + normal[2] * BAKER3D_EPS * 4};
        if (light->casts_shadows) {
            double shadow_limit = dist == DBL_MAX ? DBL_MAX : dist - BAKER3D_EPS * 8;
            double t;
            if (shadow_limit > BAKER3D_EPS &&
                baker_trace(baker, origin, to_light, shadow_limit, &t) >= 0)
                continue;
        }
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
    baker_release_node_entries(baker->nodes, baker->node_count);
    baker->node_count = 0;
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

/// @brief Snapshot one enabled non-ambient Light3D into the bake input.
/// @details Copies and sanitizes the complete punctual-light state immediately;
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
    double direction_length;
    if (!baker_inputs_mutable(baker) || baker->light_count >= BAKER3D_MAX_LIGHTS ||
        light->type < 0 || light->type > 6 || light->type == 2 || !light->enabled)
        return;
    if (light->type != 0 && (!isfinite(light->position[0]) || !isfinite(light->position[1]) ||
                             !isfinite(light->position[2])))
        return;
    baker_light *slot = &baker->lights[baker->light_count];
    memset(slot, 0, sizeof(*slot));
    slot->type = light->type;
    memcpy(slot->direction, light->direction, sizeof(slot->direction));
    memcpy(slot->position, light->position, sizeof(slot->position));
    direction_length = hypot(hypot(slot->direction[0], slot->direction[1]), slot->direction[2]);
    if ((slot->type == 0 || slot->type == 3) &&
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
    slot->decay_type = light->decay_type >= 0 && light->decay_type <= 3 ? light->decay_type : 2;
    slot->casts_shadows = light->casts_shadows ? 1 : 0;
    baker->light_count++;
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

/// @brief Convert one finite-or-overflowing world edge into a bounded chart axis.
static int32_t baker_chart_dimension(const double from[3],
                                     const double to[3],
                                     double texels_per_unit) {
    double dx = to[0] - from[0];
    double dy = to[1] - from[1];
    double dz = to[2] - from[2];
    double length = hypot(hypot(dx, dy), dz);
    double scaled = length * texels_per_unit;
    int32_t dimension;
    if (!isfinite(scaled) || scaled >= 126.0)
        return 128;
    dimension = (int32_t)ceil(scaled) + 2;
    if (dimension < 3)
        dimension = 3;
    return dimension > 128 ? 128 : dimension;
}

/// @brief Preflight and store every chart placement before any bake mutation.
static int baker_preflight_charts(rt_lightbaker3d *baker) {
    if (!baker || baker->tri_count < 0 || (baker->tri_count > 0 && !baker->tris))
        return 0;
    baker->cursor_x = 0;
    baker->cursor_y = 0;
    baker->row_height = 0;
    for (int32_t i = 0; i < baker->tri_count; ++i) {
        baker_tri *tri = &baker->tris[i];
        tri->chart_w = baker_chart_dimension(tri->p0, tri->p1, baker->texels_per_unit);
        tri->chart_h = baker_chart_dimension(tri->p0, tri->p2, baker->texels_per_unit);
        if (!baker_alloc_chart(baker, tri->chart_w, tri->chart_h, &tri->chart_x, &tri->chart_y))
            return 0;
    }
    return 1;
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

/// @brief Publish all staged chart UVs and invalidate each distinct mesh once.
static int baker_publish_uvs(rt_lightbaker3d *baker) {
    if (!baker_validate_uv_targets(baker))
        return 0;
    for (int32_t i = 0; i < baker->tri_count; ++i) {
        baker_tri *tri = &baker->tris[i];
        baker_node_entry *entry = &baker->nodes[tri->node_index];
        rt_mesh3d *mesh = (rt_mesh3d *)entry->mesh;
        double inv_dim = 1.0 / (double)baker->atlas_dim;
        float uv[3][2] = {
            {(float)((tri->chart_x + 0.5) * inv_dim), (float)((tri->chart_y + 0.5) * inv_dim)},
            {(float)((tri->chart_x + tri->chart_w - 1.5) * inv_dim),
             (float)((tri->chart_y + 0.5) * inv_dim)},
            {(float)((tri->chart_x + 0.5) * inv_dim),
             (float)((tri->chart_y + tri->chart_h - 1.5) * inv_dim)}};
        for (int corner = 0; corner < 3; ++corner) {
            vgfx3d_vertex_t *vertex = &mesh->vertices[tri->vertex_indices[corner]];
            vertex->uv1[0] = uv[corner][0];
            vertex->uv1[1] = uv[corner][1];
        }
        entry->uv1_written = 1;
    }
    for (int32_t n = 0; n < baker->node_count; ++n) {
        int already_touched = 0;
        if (!baker->nodes[n].uv1_written)
            continue;
        for (int32_t prior = 0; prior < n; ++prior) {
            if (baker->nodes[prior].uv1_written &&
                baker->nodes[prior].mesh == baker->nodes[n].mesh) {
                already_touched = 1;
                break;
            }
        }
        if (!already_touched) {
            rt_mesh3d *mesh = (rt_mesh3d *)baker->nodes[n].mesh;
            rt_mesh3d_touch_geometry(mesh);
            for (int32_t entry = n; entry < baker->node_count; ++entry)
                if (baker->nodes[entry].mesh == mesh)
                    baker->nodes[entry].geometry_revision = mesh->geometry_revision;
        }
    }
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
/// @details Each call processes at most 64 charts and updates progress. Source
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
    if ((!baker->gathered && !baker_gather_scene(baker)) || !baker_bvh_build(baker))
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

    int32_t budget = 64; /* triangles per step: keeps editor slices responsive */
    while (baker->next_tri < baker->tri_count && budget-- > 0) {
        baker_tri *tri = &baker->tris[baker->next_tri];
        int32_t w = tri->chart_w;
        int32_t h = tri->chart_h;
        int32_t cx = tri->chart_x;
        int32_t cy = tri->chart_y;
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
        baker->next_tri++;
    }
    baker->progress =
        baker->tri_count > 0 ? (double)baker->next_tri / (double)baker->tri_count : 1.0;
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
    if ((!baker->gathered && !baker_gather_scene(baker)) || !baker_bvh_build(baker))
        return;
    probes = (size_t)grid->nx * (size_t)grid->ny * (size_t)grid->nz;
    queue = (size_t *)malloc(probes * sizeof(*queue));
    if (!queue)
        return;
    grid->baked = 0;
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
            int32_t hit = baker_trace(baker, point, dir, DBL_MAX, &t);
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
                sh[c * 3 + 0] =
                    probe_accumulate_coefficient(sh[c * 3 + 0], rgb[0] * basis[c] * weight);
                sh[c * 3 + 1] =
                    probe_accumulate_coefficient(sh[c * 3 + 1], rgb[1] * basis[c] * weight);
                sh[c * 3 + 2] =
                    probe_accumulate_coefficient(sh[c * 3 + 2], rgb[2] * basis[c] * weight);
            }
        }
        grid->valid[p] = inside_hits * 4 < dir_samples ? 1 : 0;
    }
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
