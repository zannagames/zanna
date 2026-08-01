//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_internal.h
// Purpose: Internal struct definitions for Zanna.Graphics3D types.
//   Shared between rt_canvas3d.c, rt_mesh3d.c, rt_camera3d.c, etc.
//
// Key invariants:
//   - These structs are internal to the runtime; never exposed in public headers.
//   - All object pointers received from user code must be cast to these types.
//   - vgfx3d_vertex_t is internal and may evolve with renderer/importer needs.
//
// Ownership/Lifetime:
//   - Runtime objects are GC-managed unless explicitly described as stack fixtures.
//   - Internal scratch buffers are owned by their containing runtime object.
//   - Immutable mesh geometry revisions use native atomic reference ownership
//     shared by their source mesh and every Canvas3D frame that queues them.
//
// Links: rt_canvas3d.h, plans/3d/01-software-renderer.md,
//        docs/adr/0168-windowless-canvas3d-rendering.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines private Graphics3D runtime payloads and cross-module helpers.
/// @details This header is shared only by Graphics3D implementation units. It
///   centralizes renderer-facing object layouts, ownership-sensitive transient
///   storage, checked internal casts, and contracts for helpers implemented
///   across the Canvas3D, mesh, camera, material, lighting, and cubemap modules.

#pragma once

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_graphics3d_ids.h"
#include "rt_heap.h"
#include "rt_input.h"
#include "rt_postfx3d.h"
#include "rt_string.h"
#include "vgfx.h"
#include "vgfx3d_frustum.h"
#include "vgfx3d_skinning_scratch.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VGFX3D_RENDERTARGET_DIM_MAX 16384

//=============================================================================
// Vertex format
//=============================================================================

/// @brief Interleaved 92-byte vertex: position, normal, two UV sets, RGBA color, tangent
///   (with handedness in .w), and 4 bone index/weight pairs for skinning.
typedef struct {
    float pos[3];            /* object-space position */
    float normal[3];         /* vertex normal */
    float uv[2];             /* TEXCOORD_0 */
    float uv1[2];            /* TEXCOORD_1 (falls back to uv when not authored) */
    float color[4];          /* RGBA vertex color */
    float tangent[4];        /* tangent.xyz + handedness sign in tangent.w */
    uint8_t bone_indices[4]; /* bone palette indices (Phase 14) */
    float bone_weights[4];   /* blend weights (Phase 14) */
} vgfx3d_vertex_t;           /* 92 bytes */

/// @brief Compact hardware-particle instance consumed with the retained unit quad.
/// @details `center` is already in the active frame's camera-relative render space. `right` and
///   `up` are half-extent vectors, so a unit-quad corner `(x,y)` expands to
///   `center + x*right + y*up`. Four-float lanes keep the record naturally aligned and identical
///   across Metal, D3D11, and OpenGL while remaining far smaller than four expanded 92-byte
///   vertices. `center.w` is 1, `right.w` and `up.w` are zero/reserved, and `color.w` carries
///   particle alpha.
/// @note This is an internal renderer ABI, not a public runtime object layout.
typedef struct {
    float center[4];
    float right[4];
    float up[4];
    float color[4];
} vgfx3d_particle_instance_t; /* 64 bytes */

/// @brief Optional per-vertex bone influences 5-8 (palette-slot indices + weights),
///   carried as a mesh side stream so the fixed vertex record stays 92 bytes.
///   Consumed on the GPU by backends with gpu_skinning_extras (bound as a
///   per-vertex side buffer); other backends fall back to CPU skinning.
typedef struct {
    uint16_t indices[4];
    float weights[4];
} vgfx3d_extra_influences_t;

/// @brief Immutable CPU geometry retained across deferred frames for one Mesh3D revision.
/// @details Heap meshes publish one revision object lazily on their first deferred draw after a
///          mutation. The source mesh owns one reference and every canvas frame that queues the
///          revision owns another. Consequently, later mesh edits can fork to a new revision
///          without invalidating vertex/index pointers already stored in deferred commands.
///          Missing tangent data is generated into a separate immutable vertex variant so the raw
///          source copy remains unchanged and both backend upload identities can coexist.
/// @note The reference count is manipulated through `rt_mesh3d_geometry_revision_retain` and
///       `rt_mesh3d_geometry_revision_release`; callers must never update it directly.
typedef struct rt_mesh3d_geometry_revision {
    volatile int ref_count;
    uint32_t source_revision;
    uint32_t vertex_count;
    uint32_t index_count;
    vgfx3d_vertex_t *vertices;
    uint32_t *indices;
    vgfx3d_vertex_t *tangent_vertices;
    int8_t tangent_state; /* 0 = not built, 1 = ready */
} rt_mesh3d_geometry_revision;

/// @brief Notify shared spatial caches that some Mesh3D geometry changed.
void rt_mesh3d_note_global_geometry_change(void);
/// @brief Process-wide monotonic epoch for Mesh3D geometry mutations.
/// @return The current non-zero mutation epoch.
uint64_t rt_mesh3d_global_geometry_epoch(void);

//=============================================================================
// Mesh3D
//=============================================================================

/**
 * @brief One triangle-aligned material span in a Mesh3D index buffer.
 *
 * Ranges are private retained metadata used by import, clone, and simplification
 * paths. `first_index` and `index_count` are measured in indices (not triangles),
 * are multiples of three, and must describe ascending non-overlapping spans.
 * `material_slot` is the source asset's non-negative material/submesh identifier.
 */
typedef struct rt_mesh3d_submesh_range {
    uint32_t first_index;
    uint32_t index_count;
    int32_t material_slot;
} rt_mesh3d_submesh_range;

/** @brief Mesh was not produced by Mesh3D.Simplify. */
#define RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN 0
/** @brief Mesh3D.Simplify reached or surpassed its requested triangle budget. */
#define RT_MESH3D_SIMPLIFY_STATUS_COMPLETE 1
/** @brief Topology/boundary constraints stopped Mesh3D.Simplify above its target. */
#define RT_MESH3D_SIMPLIFY_STATUS_PARTIAL 2

/// @brief Mesh3D payload: growable vertex/index arrays, cached AABB/bounding-sphere,
///   a geometry revision counter, and transient skinning/morph pointers set per draw.
typedef struct {
    void *vptr;
    vgfx3d_vertex_t *vertices;
    double *positions64; /* optional authoritative double positions for AddVertex-built meshes */
    uint32_t vertex_count;
    uint32_t vertex_capacity;
    uint32_t *indices;
    uint32_t index_count;
    uint32_t index_capacity;
    rt_mesh3d_submesh_range *submesh_ranges; /* owned material/index spans */
    uint32_t submesh_range_count;
    uint32_t submesh_range_capacity;
    int64_t simplify_requested_triangles;
    int64_t simplify_achieved_triangles;
    int32_t simplify_status;            /* RT_MESH3D_SIMPLIFY_STATUS_* */
    double *normal_accum_scratch;       /* reusable vertex_count * 3 normal accumulator */
    size_t normal_accum_scratch_values; /* allocated double count for normal_accum_scratch */
    /* Transient: set by skinning path before draw, zero otherwise */
    const float *bone_palette;      /* bone_count * 16 floats (4x4 row-major) */
    const float *prev_bone_palette; /* previous-frame palette for motion blur */
    int32_t bone_count;             /* 0 = not skinned */
    /* Skin partitioning: palette slot -> skeleton bone index for sub-meshes split
     * from skins larger than VGFX3D_MAX_BONES. Owned (bone_count entries); NULL
     * means identity mapping (vertex indices address the skeleton directly). */
    int32_t *bone_map;
    /* Optional influences 5-8 per vertex (owned, vertex-count entries at import
     * time); NULL for standard 4-influence meshes. */
    vgfx3d_extra_influences_t *extra_influences;
    /* Transient: set by DrawMeshMorphed GPU path before draw, zero otherwise */
    const float *morph_deltas;        /* shape_count * vertex_count * 3 floats */
    const float *morph_normal_deltas; /* shape_count * vertex_count * 3 floats */
    const float *morph_weights;       /* shape_count floats */
    const float *prev_morph_weights;  /* previous-frame weights for motion blur */
    int32_t morph_shape_count;
    const float *morph_bound_deltas_source; /* source pointer for cached raw morph-bound padding */
    uint32_t morph_bound_revision;     /* geometry_revision for cached raw morph-bound padding */
    uint32_t morph_bound_vertex_count; /* vertex_count used by cached raw morph-bound padding */
    int32_t morph_bound_shape_count;   /* shape_count used by cached raw morph-bound padding */
    double morph_bound_pad;            /* cached max raw morph delta length */
    int8_t morph_bound_valid;
    float aabb_min[3];
    float aabb_max[3];
    float bsphere_radius;
    int8_t bounds_dirty;
    int8_t build_failed;               /* set when a construction/load append fails */
    void *skeleton_ref;                /* attached Skeleton3D (or NULL) */
    void *morph_targets_ref;           /* attached MorphTarget3D (or NULL) */
    uint32_t geometry_revision;        /* increments when CPU geometry changes */
    uint32_t tangent_revision;         /* geometry_revision for cached tangent readiness */
    int8_t tangents_ready;             /* true once tangent presence/generation was resolved */
    uint32_t validated_index_revision; /* geometry_revision for cached index validation */
    uint32_t validated_index_count;    /* complete in-range triangle-list count, 0 = invalid */
    uint32_t
        positions64_rebase_revision;  /* geometry_revision for cached double-position rebase test */
    int8_t positions64_rebase_needed; /* cached result for camera-relative vertex rebasing */
    int8_t resident;                  /* false when stream draw residency should skip this mesh */
    int8_t compact_streams;           /* opt-in: GPU static-cache uploads use the 48-byte
                                         compact vertex encoding (R20); CPU payload unchanged */
    uint8_t geometry_batch_depth;
    int8_t geometry_batch_dirty;
    int8_t transient_geometry_facade;  /* suppress global mutation notifications for stack copies */
    void *physics_bvh_nodes;           /* rt_physics_mesh_bvh_node[], owned by mesh */
    uint32_t *physics_bvh_tri_indices; /* triangle indices into indices[] / 3 */
    uint32_t physics_bvh_revision;
    int32_t physics_bvh_node_count;
    int32_t physics_bvh_tri_count;
    void *raycast_bvh_nodes;           /* retained scene-raycast BVH nodes, owned by mesh */
    uint32_t *raycast_bvh_tri_indices; /* triangle indices into indices[] / 3 */
    uint32_t raycast_bvh_revision;
    int32_t raycast_bvh_node_count;
    int32_t raycast_bvh_tri_count;
    uint64_t raycast_bvh_rebuild_count;
    uint64_t raycast_last_triangle_probe_count;
    rt_mesh3d_geometry_revision *retained_geometry;
    uint64_t retained_geometry_build_count;
    uint64_t retained_geometry_cache_hit_count;
    uint64_t retained_tangent_build_count;
    uint64_t retained_tangent_cache_hit_count;
    uint8_t retained_tangent_cache_key; /* stable address distinguishes tangent GPU uploads */
    /* Allocation generation (rt_g3d_next_identity_serial): pointer-keyed history tables
     * (motion vectors, occlusion streaks) salt their keys with this so a freed object
     * and its same-address successor never inherit each other's temporal state. */
    uint32_t identity_serial;
} rt_mesh3d;

/// @brief Return the immutable retained copy for the mesh's current geometry revision.
/// @details Reuses the current object when its revision/count tuple still matches; otherwise
///          allocates and copies the safe vertex/index ranges exactly once. The returned pointer is
///          borrowed from @p mesh and remains valid until the mesh mutates or is finalized unless
///          the caller first acquires its own reference.
/// @param mesh Source mesh whose current CPU geometry should be retained.
/// @return Borrowed revision pointer, or NULL for empty/invalid geometry or allocation failure.
rt_mesh3d_geometry_revision *rt_mesh3d_get_retained_geometry(rt_mesh3d *mesh);

/// @brief Acquire one ownership reference to an immutable mesh geometry revision.
/// @param revision Revision to retain; NULL is accepted as a no-op.
void rt_mesh3d_geometry_revision_retain(rt_mesh3d_geometry_revision *revision);

/// @brief Release one ownership reference and destroy the revision when it reaches zero.
/// @details Destruction frees the raw vertex/index copies and any lazily generated tangent vertex
///          variant. Passing NULL is a no-op.
/// @param revision Revision whose reference should be released.
void rt_mesh3d_geometry_revision_release(rt_mesh3d_geometry_revision *revision);

/// @brief Drop the mesh-owned reference to its currently retained immutable geometry.
/// @details Geometry mutation paths call this before bumping `geometry_revision`. Canvas-held
///          references keep already queued bytes alive, providing copy-on-write/fork semantics.
/// @param mesh Mesh whose current retained revision should be detached.
void rt_mesh3d_invalidate_retained_geometry(rt_mesh3d *mesh);

/// @brief Lazily build or reuse the immutable tangent-bearing vertex variant for a revision.
/// @details Tangents are derived from the revision's position, normal, UV, and index data. Because
///          all those inputs are covered by `source_revision`, an unchanged revision can safely
///          reuse the result across frames and renderer backends.
/// @param mesh Source mesh used for cache counters and revision consistency checks.
/// @param revision Immutable raw revision to augment with a tangent vertex variant.
/// @return Non-zero when `revision->tangent_vertices` is ready; zero on invalid input or failure.
int rt_mesh3d_geometry_revision_ensure_tangents(rt_mesh3d *mesh,
                                                rt_mesh3d_geometry_revision *revision);

/// @brief Allocate a Mesh3D object with all runtime bookkeeping initialized but no default
///   vertex/index storage.
/// @details This internal constructor is for import and clone paths that immediately allocate
///          exact-size buffers. Public code should continue using `rt_mesh3d_new`, which reserves
///          small growable arrays for programmatic `AddVertex` / `AddTriangle` construction.
/// @return GC-managed Mesh3D handle, or NULL on allocation failure.
void *rt_mesh3d_new_empty_storage(void);

/// @brief Vertex count safe to read directly — the live count clamped to capacity, 0 when
///   the vertex buffer is absent or empty.
/// @param mesh Borrowed mesh to inspect; may be `NULL`.
/// @return Readable vertex count clamped to allocated capacity, or zero.
static inline uint32_t rt_mesh3d_safe_vertex_count(const rt_mesh3d *mesh) {
    if (!mesh || !mesh->vertices || mesh->vertex_count == 0 || mesh->vertex_capacity == 0)
        return 0;
    return mesh->vertex_count < mesh->vertex_capacity ? mesh->vertex_count : mesh->vertex_capacity;
}

/// @brief Index count safe to read directly.
/// @details The live count is clamped to capacity, but otherwise preserved. Callers that require a
///   complete triangle-list count should use rt_mesh3d_validated_index_count() instead.
/// @param mesh Borrowed mesh to inspect; may be `NULL`.
/// @return Readable index count clamped to allocated capacity, or zero.
static inline uint32_t rt_mesh3d_safe_index_count(const rt_mesh3d *mesh) {
    if (!mesh || !mesh->indices || mesh->index_count == 0 || mesh->index_capacity == 0)
        return 0;
    return mesh->index_count < mesh->index_capacity ? mesh->index_count : mesh->index_capacity;
}

/// @brief Clamp a mesh's vertex/index counts to their safe values, marking bounds dirty
///   when either count changed.
/// @param mesh Mutable mesh to repair; `NULL` is ignored.
static inline void rt_mesh3d_repair_geometry_counts(rt_mesh3d *mesh) {
    uint32_t vertex_count;
    uint32_t index_count;
    if (!mesh)
        return;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_safe_index_count(mesh);
    if (mesh->vertex_count != vertex_count || mesh->index_count != index_count) {
        mesh->vertex_count = vertex_count;
        mesh->index_count = index_count;
        mesh->bounds_dirty = 1;
        mesh->validated_index_revision = 0;
        mesh->validated_index_count = 0;
    }
}

/// @brief Return a cached complete, in-range triangle-list index count for @p mesh.
/// @details The validation scan is paid once per geometry revision. Backends can trust a draw
///   command carrying this exact count and avoid rescanning every index in every pass. Corrupt
///   indices invalidate the cache with a zero count so consumers skip the draw safely. Revision
///   zero is reserved for stack/transient meshes and is always scanned instead of trusting the
///   zero-initialized cache stamp.
/// @param mesh Mutable mesh whose index stream and validation cache are inspected.
/// @return Complete in-range triangle-list index count, or zero for invalid or
///   empty geometry.
static inline uint32_t rt_mesh3d_validated_index_count(rt_mesh3d *mesh) {
    uint32_t vertex_count;
    uint32_t index_count;
    if (!mesh)
        return 0;
    rt_mesh3d_repair_geometry_counts(mesh);
    if (mesh->geometry_revision != 0 && mesh->validated_index_revision == mesh->geometry_revision)
        return mesh->validated_index_count;
    mesh->validated_index_revision = mesh->geometry_revision ? mesh->geometry_revision : 1u;
    mesh->validated_index_count = 0;
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    index_count = rt_mesh3d_safe_index_count(mesh);
    index_count -= index_count % 3u;
    if (!mesh->indices || vertex_count == 0 || index_count < 3u)
        return 0;
    for (uint32_t i = 0; i < index_count; ++i) {
        if (mesh->indices[i] >= vertex_count)
            return 0;
    }
    mesh->validated_index_count = index_count;
    return index_count;
}

/// @brief Allocation-free triangle-accurate mesh raycast for scene queries.
/// @details Same broad phase, retained BVH narrow phase, and singular-matrix
///          world-space fallback as `rt_ray3d_intersect_mesh`, but takes raw
///          doubles and reports the hit without boxing. Implemented in
///          rt_raycast3d.c.
/// @param origin Borrowed finite world-space ray origin.
/// @param dir Borrowed world-space direction, normalized internally.
/// @param mesh Borrowed validated mesh payload.
/// @param model Optional borrowed row-major world matrix; `NULL` = identity.
/// @param max_distance Non-negative Euclidean hit cap; negative means uncapped.
/// @param out_distance Optional output Euclidean world hit distance.
/// @param out_triangle Optional output winning triangle index.
/// @param out_point Optional output world-space hit point (double[3]).
/// @param out_normal Optional output normalized world face normal (double[3]).
/// @return Nonzero when a triangle hit within range was found.
int rt_raycast3d_mesh_hit_raw(const double origin[3],
                              const double dir[3],
                              rt_mesh3d *mesh,
                              const double *model,
                              double max_distance,
                              double *out_distance,
                              int64_t *out_triangle,
                              double *out_point,
                              double *out_normal);

/// @brief Box a RayHit3D from raw hit data. Implemented in rt_raycast3d.c,
///        which privately owns the RayHit3D payload layout.
/// @param distance Non-negative Euclidean world hit distance.
/// @param point Borrowed world-space hit point (double[3]).
/// @param normal Borrowed normalized world face normal (double[3]).
/// @param triangle Winning triangle index within the hit mesh.
/// @return New RayHit3D object, or `NULL` on invalid input or allocation failure.
void *rt_raycast3d_build_hit(double distance,
                             const double point[3],
                             const double normal[3],
                             int64_t triangle);

/// @brief Zero a mesh's cached AABB/bounding-sphere and clear the dirty flag.
/// @param mesh Mutable mesh whose cached bounds are reset; `NULL` is ignored.
static inline void rt_mesh3d_reset_bounds(rt_mesh3d *mesh) {
    if (!mesh)
        return;
    mesh->aabb_min[0] = mesh->aabb_min[1] = mesh->aabb_min[2] = 0.0f;
    mesh->aabb_max[0] = mesh->aabb_max[1] = mesh->aabb_max[2] = 0.0f;
    mesh->bsphere_radius = 0.0f;
    mesh->bounds_dirty = 0;
}

/// @brief Flag a mesh's cached bounds as stale (recomputed lazily on next use).
/// @param mesh Mutable mesh to invalidate; `NULL` is ignored.
static inline void rt_mesh3d_mark_bounds_dirty(rt_mesh3d *mesh) {
    if (mesh)
        mesh->bounds_dirty = 1;
}

/// @brief Clamp a double-precision mesh coordinate into the float range used by backend AABBs.
/// @details Meshes authored through the runtime can retain authoritative double positions while
///   their GPU vertices store narrowed floats. Bounds should be derived from the double positions
///   so culling remains conservative for large worlds, then clamped to the backend float domain.
/// @param value Double-precision coordinate to sanitize.
/// @return A finite float: zero for non-finite input, otherwise @p value
///   clamped to the inclusive float range.
static inline float rt_mesh3d_bounds_f32_from_f64(double value) {
    if (!isfinite(value))
        return 0.0f;
    if (value > (double)FLT_MAX)
        return FLT_MAX;
    if (value < (double)-FLT_MAX)
        return -FLT_MAX;
    return (float)value;
}

/// @brief Immediately mark geometry changed: dirties bounds, bumps geometry_revision (wrapping
///        past UINT32_MAX to 1), and invalidates cached tangents. Bypasses batch deferral.
/// @param mesh Mutable mesh whose geometry-dependent caches are invalidated;
///   `NULL` is ignored.
static inline void rt_mesh3d_touch_geometry_now(rt_mesh3d *mesh) {
    if (!mesh)
        return;
    if (!mesh->transient_geometry_facade)
        rt_mesh3d_invalidate_retained_geometry(mesh);
    mesh->resident = 1;
    mesh->bounds_dirty = 1;
    if (mesh->geometry_revision == UINT32_MAX)
        mesh->geometry_revision = 1;
    else
        mesh->geometry_revision++;
    mesh->tangents_ready = 0;
    mesh->tangent_revision = 0;
    mesh->validated_index_revision = 0;
    mesh->validated_index_count = 0;
    mesh->positions64_rebase_revision = 0;
    mesh->positions64_rebase_needed = 0;
    mesh->morph_bound_deltas_source = NULL;
    mesh->morph_bound_revision = 0;
    mesh->morph_bound_vertex_count = 0;
    mesh->morph_bound_shape_count = 0;
    mesh->morph_bound_pad = 0.0;
    mesh->morph_bound_valid = 0;
    mesh->simplify_requested_triangles = 0;
    mesh->simplify_achieved_triangles = 0;
    mesh->simplify_status = RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN;
    if (!mesh->transient_geometry_facade)
        rt_mesh3d_note_global_geometry_change();
}

/// @brief Mark geometry changed: dirties bounds and bumps geometry_revision
///        (wrapping past UINT32_MAX to 1) so GPU buffers know to re-upload.
/// @param mesh Mutable mesh to touch; a live edit batch defers the actual
///   invalidation until its outermost end.
static inline void rt_mesh3d_touch_geometry(rt_mesh3d *mesh) {
    if (!mesh)
        return;
    if (mesh->geometry_batch_depth > 0) {
        mesh->geometry_batch_dirty = 1;
        return;
    }
    rt_mesh3d_touch_geometry_now(mesh);
}

/// @brief Open a geometry-edit batch: defers per-edit revision bumps until the batch ends.
/// @details Re-entrant via a depth counter (saturates at UINT8_MAX), so bulk vertex edits trigger
///          a single re-upload instead of one per change. Pair with rt_mesh3d_end_geometry_batch.
/// @param mesh Mutable mesh whose edit-batch depth is incremented; `NULL` and
///   saturated depth are ignored.
static inline void rt_mesh3d_begin_geometry_batch(rt_mesh3d *mesh) {
    if (!mesh || mesh->geometry_batch_depth == UINT8_MAX)
        return;
    mesh->geometry_batch_depth++;
}

/// @brief Close a geometry-edit batch; when the outermost batch closes and edits occurred,
///        applies a single deferred geometry touch.
/// @param mesh Mutable mesh whose edit-batch depth is decremented; `NULL` and
///   an already-zero depth are ignored.
static inline void rt_mesh3d_end_geometry_batch(rt_mesh3d *mesh) {
    if (!mesh || mesh->geometry_batch_depth == 0)
        return;
    mesh->geometry_batch_depth--;
    if (mesh->geometry_batch_depth == 0 && mesh->geometry_batch_dirty) {
        mesh->geometry_batch_dirty = 0;
        rt_mesh3d_touch_geometry_now(mesh);
    }
}

/// @brief Recompute a mesh's AABB/bounding sphere if marked dirty.
/// @details No-op when clean; resets to zero bounds when the mesh has no
///          vertices; otherwise calls vgfx3d_compute_mesh_aabb over the
///          vertex buffer and clears the dirty flag.
/// @param mesh Mutable mesh whose cached bounds are refreshed when dirty.
static inline void rt_mesh3d_refresh_bounds(rt_mesh3d *mesh) {
    uint32_t vertex_count;
    if (!mesh || !mesh->bounds_dirty)
        return;
    rt_mesh3d_repair_geometry_counts(mesh);
    vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    if (!mesh->vertices || vertex_count == 0) {
        rt_mesh3d_reset_bounds(mesh);
        return;
    }
    if (mesh->positions64) {
        double minv[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
        double maxv[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
        for (uint32_t i = 0; i < vertex_count; i++) {
            const double *p = &mesh->positions64[(size_t)i * 3u];
            for (int axis = 0; axis < 3; axis++) {
                double value = isfinite(p[axis]) ? p[axis] : 0.0;
                if (value < minv[axis])
                    minv[axis] = value;
                if (value > maxv[axis])
                    maxv[axis] = value;
            }
        }
        for (int axis = 0; axis < 3; axis++) {
            mesh->aabb_min[axis] = rt_mesh3d_bounds_f32_from_f64(minv[axis]);
            mesh->aabb_max[axis] = rt_mesh3d_bounds_f32_from_f64(maxv[axis]);
        }
    } else {
        vgfx3d_compute_mesh_aabb(
            mesh->vertices, vertex_count, sizeof(vgfx3d_vertex_t), mesh->aabb_min, mesh->aabb_max);
    }
    if (!isfinite(mesh->aabb_min[0]) || !isfinite(mesh->aabb_min[1]) ||
        !isfinite(mesh->aabb_min[2]) || !isfinite(mesh->aabb_max[0]) ||
        !isfinite(mesh->aabb_max[1]) || !isfinite(mesh->aabb_max[2])) {
        rt_mesh3d_reset_bounds(mesh);
        return;
    }
    {
        double dx = (double)mesh->aabb_max[0] - (double)mesh->aabb_min[0];
        double dy = (double)mesh->aabb_max[1] - (double)mesh->aabb_min[1];
        double dz = (double)mesh->aabb_max[2] - (double)mesh->aabb_min[2];
        double radius = 0.5 * sqrt(dx * dx + dy * dy + dz * dz);
        if (!isfinite(radius) || radius < 0.0)
            radius = (double)FLT_MAX;
        mesh->bsphere_radius = radius > (double)FLT_MAX ? FLT_MAX : (float)radius;
    }
    mesh->bounds_dirty = 0;
}

//=============================================================================
// Camera3D
//=============================================================================

/// @brief Camera3D payload: cached view/projection matrices, eye position, perspective
///   or ortho parameters, FPS yaw/pitch, and camera-shake state.
typedef struct {
    void *vptr;
    double view[16];       /* view matrix, row-major */
    double projection[16]; /* projection matrix, row-major */
    double eye[3];         /* camera world position */
    double fov;
    double aspect;
    double near_plane;
    double far_plane;
    double fps_yaw;   /* FPS mode: horizontal rotation (degrees) */
    double fps_pitch; /* FPS mode: vertical rotation (degrees, clamped ±89) */
    /* Camera shake state */
    double shake_intensity;
    double shake_duration;
    double shake_decay;
    double shake_offset[3];
    uint32_t shake_seed;
    int64_t last_shake_update_token; /* renderer timing token for one shake advance per frame */
    int8_t is_ortho;                 /* 1 = orthographic projection */
    double ortho_size;               /* half-extent of ortho view */
    int8_t pick_cache_valid;
    int8_t pick_cache_is_ortho;
    double pick_cache_aspect;
    double pick_cache_fov;
    double pick_cache_near;
    double pick_cache_far;
    double pick_cache_ortho_size;
    double pick_cache_view[16];
    double pick_cache_inv_vp[16];
} rt_camera3d;

/// @brief Update a camera's cached projection for the given viewport aspect.
/// @param cam Borrowed Camera3D handle; invalid handles are ignored.
/// @param aspect Positive viewport width-to-height ratio.
void rt_camera3d_sync_render_aspect(void *cam, double aspect);
/// @brief Set the retained perspective FOV even while an orthographic projection is active.
/// @param cam Borrowed Camera3D handle; invalid handles are ignored.
/// @param fov Vertical perspective field of view in degrees.
void rt_camera3d_set_retained_fov(void *cam, double fov);
/// @brief Compute a camera's 4x4 projection matrix into @p out_projection,
///        optionally overriding the aspect ratio (<= 0 keeps the camera's).
/// @param cam Borrowed Camera3D handle supplying projection parameters.
/// @param aspect_override Positive width-to-height override, or non-positive to
///   use the retained camera aspect.
/// @param out_projection Non-`NULL` output array receiving 16 row-major floats.
void rt_camera3d_get_render_projection(void *cam, double aspect_override, float *out_projection);
/// @brief Internal: advance camera shake by @p dt seconds and refresh the shaken view.
/// @param cam Borrowed Camera3D handle to update.
/// @param dt Non-negative elapsed time in seconds.
void rt_camera3d_update_shake_for_frame(void *cam, double dt);
/// @brief Internal: advance camera shake at most once for a renderer timing token.
/// @param cam Borrowed Camera3D handle to update.
/// @param dt Non-negative elapsed time in seconds.
/// @param frame_token Renderer frame identity used to suppress duplicate advances.
void rt_camera3d_update_shake_for_frame_token(void *cam, double dt, int64_t frame_token);
/// @brief Internal deep copy used when instantiating scene-template cameras.
/// @param cam Borrowed live Camera3D to copy.
/// @return New GC-managed Camera3D with independent cached state, or `NULL` on failure.
void *rt_camera3d_clone(void *cam);
/// @brief Internal scalar look-at path used by importers and scene-node camera coupling.
/// @param obj Borrowed Camera3D handle to orient.
/// @param eye_x Camera world-position X component.
/// @param eye_y Camera world-position Y component.
/// @param eye_z Camera world-position Z component.
/// @param target_x Look target X component.
/// @param target_y Look target Y component.
/// @param target_z Look target Z component.
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

/// @brief Internal Mesh3D tangent generator for already-validated mesh storage.
/// @param mesh Mutable Mesh3D whose tangent lanes and geometry revision are updated.
void rt_mesh3d_calc_tangents_impl(rt_mesh3d *mesh);

/// @brief One material-group result produced while importing an OBJ mesh.
/// @details The importer owns both the NUL-terminated material name and retained
///   Mesh3D handle until rt_mesh3d_obj_groups_free() releases the array.
typedef struct {
    char *material_name;
    void *mesh;
} rt_mesh3d_obj_group_t;

/// @brief Load an OBJ file into independently material-addressable mesh groups.
/// @param path Borrowed runtime path string naming the OBJ input.
/// @param out_groups Non-`NULL` output receiving an owned group array on success.
/// @param out_count Non-`NULL` output receiving the number of array entries.
/// @return Non-zero on successful parsing, including an empty valid result;
///   zero on I/O, parse, validation, or allocation failure.
int rt_mesh3d_from_obj_groups(rt_string path,
                              rt_mesh3d_obj_group_t **out_groups,
                              int32_t *out_count);
/// @brief Release an OBJ material-group array and every name/mesh it owns.
/// @param groups Owned array returned by rt_mesh3d_from_obj_groups(); `NULL` is accepted.
/// @param count Number of initialized entries in @p groups.
void rt_mesh3d_obj_groups_free(rt_mesh3d_obj_group_t *groups, int32_t count);

//=============================================================================
// Material3D
//=============================================================================

#define RT_MATERIAL3D_TEXTURE_SLOT_BASE_COLOR 0
#define RT_MATERIAL3D_TEXTURE_SLOT_NORMAL 1
#define RT_MATERIAL3D_TEXTURE_SLOT_SPECULAR 2
#define RT_MATERIAL3D_TEXTURE_SLOT_EMISSIVE 3
#define RT_MATERIAL3D_TEXTURE_SLOT_METALLIC_ROUGHNESS 4
#define RT_MATERIAL3D_TEXTURE_SLOT_AO 5
#define RT_MATERIAL3D_TEXTURE_SLOT_COUNT 6

/// @brief Material3D payload: diffuse/specular/emissive colors, PBR metallic-roughness
///   factors, six texture-map slots with per-slot sampler/UV state, alpha/blend mode,
///   environment reflectivity, shading model, and custom shader params.
typedef struct {
    void *vptr;
    double diffuse[4]; /* RGBA diffuse color */
    double specular[3];
    double shininess;
    int32_t workflow; /* 0=legacy/Blinn-Phong surface, 1=PBR metallic-roughness */
    void *texture;    /* Pixels, TextureAsset3D, or RenderTarget3D source (diffuse, slot 0) */
    void *normal_map; /* Pixels, TextureAsset3D, or RenderTarget3D source (normal map, slot 1) */
    void
        *specular_map; /* Pixels, TextureAsset3D, or RenderTarget3D source (specular map, slot 2) */
    void
        *emissive_map; /* Pixels, TextureAsset3D, or RenderTarget3D source (emissive map, slot 3) */
    void *metallic_roughness_map; /* Pixels, TextureAsset3D, or RenderTarget3D source (glTF
                                     metallic/roughness map) */
    void *ao_map;   /* Pixels, TextureAsset3D, or RenderTarget3D source (ambient occlusion map) */
    void *lightmap; /* baked GI atlas (Pixels), sampled with TEXCOORD_1; replaces flat ambient */
    double emissive[3];        /* emissive color multiplier */
    double metallic;           /* [0,1] dielectric->metal */
    double roughness;          /* [0,1] smooth->rough */
    double ao;                 /* [0,1] ambient occlusion multiplier */
    double emissive_intensity; /* scalar multiplier applied after emissive color/map */
    double normal_scale;       /* scales tangent-space XY perturbation */
    double alpha;              /* opacity [0.0=invisible, 1.0=opaque], default 1.0 */
    double alpha_cutoff;       /* alpha-mask cutoff, default 0.5 */
    void *env_map;             /* CubeMap3D for environment reflections (or NULL) */
    double reflectivity;       /* [0.0=no reflection, 1.0=mirror], default 0.0 */
    int8_t unlit;
    int8_t double_sided;
    int8_t additive_blend;  /* internal-only: route through additive blend state when true */
    int32_t alpha_mode;     /* 0=opaque, 1=mask, 2=blend */
    int8_t alpha_mode_auto; /* true when SetAlpha auto-promoted OPAQUE -> BLEND */
    int32_t shadow_mode;    /* 0=auto, 1=none, 2=cast even when alpha-blended */
    int32_t texture_wrap_s; /* RT_MATERIAL3D_TEXTURE_WRAP_* for imported material textures */
    int32_t texture_wrap_t;
    int32_t texture_filter;     /* RT_MATERIAL3D_TEXTURE_FILTER_* */
    int32_t texture_min_filter; /* independent imported minification filter */
    int32_t texture_mag_filter; /* independent imported magnification filter */
    int32_t texture_mip_filter; /* RT_MATERIAL3D_TEXTURE_MIP_FILTER_* */
    int32_t anisotropy;         /* 1=off, otherwise clamped to [1,16] */
    int32_t texture_slot_wrap_s[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_wrap_t[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_min_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_mag_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_mip_filter[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_anisotropy[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    int32_t texture_slot_uv_set[RT_MATERIAL3D_TEXTURE_SLOT_COUNT];
    double texture_slot_uv_transform[RT_MATERIAL3D_TEXTURE_SLOT_COUNT][6];
    int32_t shading_model;    /* 0=BlinnPhong, 1=Toon, 2=PBR, 3=Unlit, 4=Fresnel, 5=Emissive */
    double custom_params[12]; /* user-defined parameters per shading model */
    double depth_bias;        /* constant depth offset; negative pulls coplanar geometry forward */
    double slope_scaled_depth_bias; /* additional slope-scaled depth offset for decals/overlays */
    double soft_fade;               /* soft-particle fade distance in world units (0 = off) */
    int8_t ssr_enabled;             /* screen-space reflections opt-in (Plan 10) */
    uint32_t identity_serial;       /* allocation generation (see rt_mesh3d.identity_serial) */
} rt_material3d;

/// @brief Monotonic allocation generation for pointer-keyed history salting
///   (rt_canvas3d_motion.c). Never returns 0; main-thread only, like object creation.
/// @return A new non-zero process-local object identity serial.
uint32_t rt_g3d_next_identity_serial(void);

/// @brief Resolve a Material3D texture slot source to the currently resident Pixels fallback.
/// @param texture_ref Borrowed Pixels, TextureAsset3D, RenderTarget3D, or `NULL`.
/// @return Borrowed resident Pixels backing the reference, or `NULL` when unavailable.
void *rt_material3d_resolve_texture_pixels(void *texture_ref);
/// @brief Resolve a Material3D texture slot source to a native TextureAsset3D, if any.
/// @param texture_ref Borrowed texture source to inspect.
/// @return Borrowed TextureAsset3D handle, or `NULL` for other source kinds.
void *rt_material3d_resolve_texture_native_asset(void *texture_ref);
/// @brief Private persistence/fidelity slot index for the baked lightmap reference.
#define RT_MATERIAL3D_PERSISTED_TEXTURE_SLOT_LIGHTMAP 6
#define RT_MATERIAL3D_PERSISTED_TEXTURE_SLOT_COUNT 7
/// @brief Borrow one original material texture reference without resolving it to Pixels.
/// @details Slots 0..5 use RT_MATERIAL3D_TEXTURE_SLOT_* ordering; slot 6 is the lightmap.
/// @param obj Borrowed Material3D handle.
/// @param slot Persisted texture slot in the inclusive range zero through six.
/// @return Borrowed original texture reference, or `NULL` for invalid input or an empty slot.
void *rt_material3d_get_persisted_texture_ref(void *obj, int64_t slot);

#define RT_MATERIAL3D_TEXTURE_WRAP_REPEAT 0
#define RT_MATERIAL3D_TEXTURE_WRAP_CLAMP_TO_EDGE 1
#define RT_MATERIAL3D_TEXTURE_WRAP_MIRRORED_REPEAT 2

#define RT_MATERIAL3D_TEXTURE_FILTER_LINEAR 0
#define RT_MATERIAL3D_TEXTURE_FILTER_NEAREST 1

#define RT_MATERIAL3D_TEXTURE_MIP_FILTER_NONE 0
#define RT_MATERIAL3D_TEXTURE_MIP_FILTER_NEAREST 1
#define RT_MATERIAL3D_TEXTURE_MIP_FILTER_LINEAR 2

/**
 * @brief Store independently authored texture sampler axes and UV metadata on one material slot.
 * @details This implementation-only importer boundary preserves glTF's independent minification,
 *          magnification, and mip-selection controls through Material3D state. Invalid material
 *          handles or slot indices are ignored. Numeric UV transforms and enum values are
 *          sanitized before publication, and the base-color slot also refreshes the legacy
 *          material-wide mirrors.
 * @param obj Borrowed Material3D runtime handle; ownership is unchanged.
 * @param slot Texture slot in `[0, RT_MATERIAL3D_TEXTURE_SLOT_COUNT)`.
 * @param uv_set Authored UV stream index; importers must validate representability first.
 * @param offset_u Horizontal UV translation.
 * @param offset_v Vertical UV translation.
 * @param scale_u Horizontal UV scale.
 * @param scale_v Vertical UV scale.
 * @param rotation Counter-clockwise UV rotation in radians.
 * @param wrap_s Horizontal `RT_MATERIAL3D_TEXTURE_WRAP_*` value.
 * @param wrap_t Vertical `RT_MATERIAL3D_TEXTURE_WRAP_*` value.
 * @param min_filter Independent `RT_MATERIAL3D_TEXTURE_FILTER_*` minification value.
 * @param mag_filter Independent `RT_MATERIAL3D_TEXTURE_FILTER_*` magnification value.
 * @param mip_filter Independent `RT_MATERIAL3D_TEXTURE_MIP_FILTER_*` selector.
 */
void rt_material3d_set_import_texture_slot_sampler_axes(void *obj,
                                                        int64_t slot,
                                                        int64_t uv_set,
                                                        double offset_u,
                                                        double offset_v,
                                                        double scale_u,
                                                        double scale_v,
                                                        double rotation,
                                                        int64_t wrap_s,
                                                        int64_t wrap_t,
                                                        int64_t min_filter,
                                                        int64_t mag_filter,
                                                        int64_t mip_filter);

//=============================================================================
// Light3D
//=============================================================================

/// @brief Light3D payload: punctual/ambient/area/volume kind, pose and emitter basis,
///   color/intensity/decay, dimensions/range, and shadow eligibility.
typedef struct {
    void *vptr;
    int32_t type; /* 0=directional, 1=point, 2=ambient, 3=spot, 4=rect, 5=sphere, 6=volume */
    double direction[3];
    double position[3];
    double color[3];
    double intensity;
    double attenuation;
    double inner_cos; /* spot light inner cone cosine (full brightness inside) */
    double outer_cos; /* spot light outer cone cosine (zero brightness outside) */
    double basis_u[3];
    double basis_v[3];
    double width;
    double height;
    double radius;
    double range;
    int32_t decay_type; /* 0=none, 1=linear, 2=quadratic, 3=cubic */
    int8_t enabled;
    int8_t casts_shadows;
} rt_light3d;

/* Minimum local-light attenuation used by constructors and importers to avoid unbounded point,
 * spot, rectangle, and sphere lights. Directional and ambient lights keep zero attenuation
 * because they are not distance-falloff lights. */
#define RT_LIGHT3D_DEFAULT_ATTENUATION 0.001

//=============================================================================
// Canvas3D
//=============================================================================

#define VGFX3D_FORWARD_LIGHT_LIMIT 16
#define VGFX3D_MAX_LIGHTS 64
/* Total shadow slots: 0..3 are per-texture slots (CSM cascades and/or the
 * highest-priority lights); 4..11 are tiles of the GPU backends' internal
 * shadow atlas (software keeps per-slot buffers). */
#define VGFX3D_MAX_SHADOW_LIGHTS 12
/* CSM cascade slot count. Cascade-semantic sizes (split arrays, cascade
 * clamps) key on this, NOT on the total shadow-slot count, so growing the
 * general shadow budget cannot silently widen the float4 cascade-split
 * payload the shaders consume. */
#define VGFX3D_CSM_SLOTS 4
#define RT_CANVAS3D_EVENT_QUEUE_CAPACITY 128
#define RT_CANVAS3D_MESH_SNAPSHOT_FRAME_BYTE_BUDGET (256ull * 1024ull * 1024ull)
#define RT_CANVAS3D_WORLD_BOUNDS_CACHE_SIZE 1024

typedef struct {
    void *source;
    uint32_t geometry_revision;
    uint32_t vertex_count;
    uint32_t index_count;
    vgfx3d_vertex_t *vertices;
    uint32_t *indices;
    int8_t tangents_generated;
    rt_mesh3d_geometry_revision *retained_revision;
} rt_canvas3d_mesh_snapshot_entry;

/// @brief Per-frame cache entry for copied float payloads used by deferred draw commands.
/// @details Morph weights, raw morph deltas, and bone palettes are still validated against their
/// source arrays on every queued draw, but identical source/count/content tuples in one frame can
/// share a single temp-buffer snapshot. This removes repeated heap copies when a skinned or morphed
/// mesh is submitted more than once before frame cleanup.
typedef struct {
    const float *source;
    size_t count;
    uint64_t content_hash;
    float *snapshot;
} rt_canvas3d_float_snapshot_entry;

/// @brief Persistent raster cache entry for one DrawText2DAA result.
/// @details The legacy AA path rasterizes into Pixels and submits a textured overlay quad. Keeping
///          the Pixels alive across frames prevents unchanged HUD labels from allocating and
///          uploading a fresh GPU texture on every draw. Entries own both @p text and @p pixels;
///          the canvas releases them on LRU eviction or teardown.
typedef struct {
    char *text;
    size_t text_len;
    void *pixels;
    size_t retained_bytes;
    double scale;
    int64_t color;
    int64_t last_used_frame;
    int32_t width;
    int32_t height;
} rt_canvas3d_aa_text_cache_entry;

/// @brief CPU occlusion history entry keyed by stable draw identity.
/// @details Coarse occlusion culling is intentionally delayed for objects that have only just
///          become covered. Requiring repeated covered results prevents one-frame projected-AABB
///          mistakes from blinking visible triangles out of the scene.
typedef struct {
    uintptr_t key;
    int32_t covered_streak;
    int64_t last_frame_seen;
} rt_canvas3d_occlusion_history_entry;

/// @brief Per-frame duplicate counter for queued draws sharing one occlusion fingerprint.
/// @details CPU occlusion history needs distinct keys for identical repeated submissions. The
///          deferred draw queue is finalized in one pass, and this table tracks how many times each
///          fingerprint has already appeared in the current frame.
typedef struct {
    uintptr_t fingerprint;
    int32_t count;
} rt_canvas3d_occlusion_duplicate_entry;

/// @brief One fixed-slot per-frame world-AABB cache entry.
/// @details The hash narrows lookup, but hits still compare the full mesh pointer plus all
///          sixteen source double matrix components so collisions cannot affect correctness.
typedef struct {
    const void *mesh_key;
    uint64_t matrix_hash;
    double matrix_key[16];
    float world_min[3];
    float world_max[3];
    int8_t occupied;
} rt_canvas3d_world_bounds_cache_entry;

/* Forward declaration — defined in vgfx3d_backend.h */
typedef struct vgfx3d_backend vgfx3d_backend_t;

//=============================================================================
// CubeMap3D — 6-face cube map texture for skybox + reflections
//=============================================================================

/// @brief Number of prefiltered specular mip levels retained for image-based
///   lighting (base 128 halving to 4: 128/64/32/16/8/4).
#define RT_CUBEMAP3D_IBL_MAX_MIPS 6

/// @brief CubeMap3D payload: six square Pixels faces (±X, ±Y, ±Z) plus a cache identity
///   used as a stable GPU-upload key across allocator reuse. When image-based
///   lighting has been prepared (rt_cubemap3d_ensure_ibl), the payload also
///   carries SH-9 irradiance coefficients and a GGX-prefiltered specular mip
///   chain stored as additional Pixels faces.
typedef struct {
    void *vptr;
    void *faces[6];          /* Pixels objects: +X, -X, +Y, -Y, +Z, -Z */
    int64_t face_size;       /* width = height per face (must be square) */
    uint64_t cache_identity; /* stable cache key generation across allocator reuse */
    /* --- IBL payload (valid only when ibl_ready != 0) --- */
    float ibl_sh[27]; /* SH-9 RGB irradiance, cosine-convolved, 1/pi folded */
    void *ibl_mips[RT_CUBEMAP3D_IBL_MAX_MIPS][6]; /* prefiltered Pixels faces  */
    int32_t ibl_mip_count;                        /* 0 until rt_cubemap3d_ensure_ibl */
    int32_t ibl_base_size;                        /* face size of prefiltered mip 0 */
    int8_t ibl_ready;
    uint64_t ibl_identity; /* distinct GPU cache key for the prefiltered chain */
} rt_cubemap3d;

#ifdef __cplusplus
extern "C" {
#endif
/// @brief Return 1 when @p cubemap is a live CubeMap3D with all six matching square faces.
/// @param cubemap Borrowed candidate CubeMap3D runtime handle.
/// @return One when all six faces are present, square, and dimensionally
///   consistent; otherwise zero.
int rt_cubemap3d_is_complete(void *cubemap);
#ifdef __cplusplus
}
#endif

//=============================================================================
// RenderTarget3D — offscreen color + depth buffers
//=============================================================================

typedef struct vgfx3d_rendertarget vgfx3d_rendertarget_t;
/// @brief Callback that refreshes a render target's CPU color mirror from backend storage.
/// @param userdata Borrowed backend-defined callback context.
/// @param target Borrowed live target whose CPU mirror must be updated.
/// @return Non-zero on successful synchronization; zero on failure.
typedef int (*vgfx3d_rendertarget_sync_fn)(void *userdata, vgfx3d_rendertarget_t *target);
/// @brief Callback that detaches a render target from its owning backend cache.
/// @param userdata Borrowed backend-defined callback context.
/// @param target Borrowed target shell that remains valid throughout the callback.
typedef void (*vgfx3d_rendertarget_release_fn)(void *userdata, vgfx3d_rendertarget_t *target);

/// @brief Render-target color format: 8-bit UNORM (LDR) or 16-bit float (HDR).
typedef enum {
    VGFX3D_RENDERTARGET_COLOR_FORMAT_UNORM8 = 0,
    VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F = 1,
} vgfx3d_rendertarget_color_format_t;

/// @brief Offscreen render target: lazily-allocated LDR/HDR color and depth buffers,
///   dimensions/stride/format, dirty flags, and a backend color-sync callback that
///   refreshes the CPU mirror from a GPU surface on readback.
struct vgfx3d_rendertarget {
    uint8_t *color_buf;   /* RGBA pixels (software path) */
    float *hdr_color_buf; /* linear RGBA32F CPU mirror for HDR GPU readback */
    float *depth_buf;     /* float depth buffer */
    int32_t width;
    int32_t height;
    int32_t stride; /* width * 4 */
    int32_t color_format;
    uint64_t cache_identity;  /* stable key generation across allocator pointer reuse */
    uint64_t estimated_bytes; /* native storage + all lazy CPU mirrors reserved against budget */
    int8_t color_dirty;
    int8_t hdr_color_valid;
    /* Bumped when a Canvas3D frame that rendered into this target ends; lets
     * RT-as-material-texture mirrors refresh only on real content changes. */
    uint64_t content_revision;
    vgfx3d_rendertarget_sync_fn sync_color;
    void *sync_color_userdata;
    /* Optional native-cache owner notification. The render-target finalizer invokes this while
     * the shell is still live, allowing a backend to discard entries that borrow `target`. */
    vgfx3d_rendertarget_release_fn release_backend;
    void *release_backend_userdata;
};

/// @brief True if the render target uses the HDR (16-bit float) color format.
/// @param target Borrowed render target to inspect; may be `NULL`.
/// @return Non-zero only for a non-`NULL` HDR-format target.
static inline int vgfx3d_rendertarget_is_hdr(const vgfx3d_rendertarget_t *target) {
    return target && target->color_format == VGFX3D_RENDERTARGET_COLOR_FORMAT_HDR16F;
}

/// @brief Validate the render target's dimensions and compute its pixel count into
///   *out_pixel_count; returns 0 (count 0) on degenerate, oversized, or overflowing sizes.
/// @param target Borrowed render target whose dimensions are validated.
/// @param out_pixel_count Optional output initialized to zero and set to the
///   validated width-times-height product on success.
/// @return One for valid bounded dimensions whose product fits `size_t`; otherwise zero.
static inline int vgfx3d_rendertarget_valid_pixels(const vgfx3d_rendertarget_t *target,
                                                   size_t *out_pixel_count) {
    size_t pixels;
    if (out_pixel_count)
        *out_pixel_count = 0u;
    if (!target || target->width <= 0 || target->height <= 0)
        return 0;
    if (target->width > VGFX3D_RENDERTARGET_DIM_MAX || target->height > VGFX3D_RENDERTARGET_DIM_MAX)
        return 0;
    if ((size_t)target->width > SIZE_MAX / (size_t)target->height)
        return 0;
    pixels = (size_t)target->width * (size_t)target->height;
    if (out_pixel_count)
        *out_pixel_count = pixels;
    return 1;
}

/// @brief Validate the render target's color stride against its width/format and compute the
///   color buffer byte size into *out_bytes; returns 0 on an invalid or overflowing layout.
/// @param target Borrowed render target whose dimensions and RGBA stride are validated.
/// @param out_bytes Optional output initialized to zero and set to total color bytes on success.
/// @return One when the color layout is valid and its byte size fits `size_t`; otherwise zero.
static inline int vgfx3d_rendertarget_valid_color_layout(const vgfx3d_rendertarget_t *target,
                                                         size_t *out_bytes) {
    size_t min_stride;
    size_t bytes;
    if (out_bytes)
        *out_bytes = 0u;
    if (!target || target->stride <= 0)
        return 0;
    if (!vgfx3d_rendertarget_valid_pixels(target, NULL))
        return 0;
    if ((size_t)target->width > SIZE_MAX / 4u)
        return 0;
    min_stride = (size_t)target->width * 4u;
    if ((size_t)target->stride < min_stride)
        return 0;
    if ((size_t)target->height > SIZE_MAX / (size_t)target->stride)
        return 0;
    bytes = (size_t)target->height * (size_t)target->stride;
    if (out_bytes)
        *out_bytes = bytes;
    return 1;
}

/// @brief Lazily allocate the 8-bit LDR color buffer (zero-filled).
/// @details No-op if already allocated; fails on invalid dims or overflow.
/// @param target Mutable render target that owns the resulting allocation.
/// @return 1 if the buffer is available, 0 on failure.
static inline int vgfx3d_rendertarget_ensure_color(vgfx3d_rendertarget_t *target) {
    size_t bytes;
    if (!target)
        return 0;
    if (!vgfx3d_rendertarget_valid_color_layout(target, &bytes))
        return 0;
    if (target->color_buf)
        return 1;
    target->color_buf = (uint8_t *)calloc(bytes, 1u);
    return target->color_buf != NULL;
}

/// @brief Lazily allocate the RGBA float HDR color buffer (zero-filled).
/// @details Only valid for HDR-format targets; fails otherwise or on overflow.
/// @param target Mutable HDR render target that owns the resulting allocation.
/// @return 1 if the HDR buffer is available, 0 on failure.
static inline int vgfx3d_rendertarget_ensure_hdr_color(vgfx3d_rendertarget_t *target) {
    size_t pixel_count;
    size_t float_count;
    if (!vgfx3d_rendertarget_is_hdr(target))
        return 0;
    if (!vgfx3d_rendertarget_valid_pixels(target, &pixel_count))
        return 0;
    if (pixel_count > SIZE_MAX / (sizeof(float) * 4u))
        return 0;
    if (target->hdr_color_buf)
        return 1;
    float_count = pixel_count * 4u;
    target->hdr_color_buf = (float *)calloc(float_count, sizeof(float));
    return target->hdr_color_buf != NULL;
}

/// @brief Flood a depth buffer with FLT_MAX (the "infinitely far" clear value) using
///   exponential-doubling memcpy — write one float, then repeatedly copy the filled
///   prefix over the rest, doubling each pass (memset can't write a 4-byte pattern).
/// @param depth Mutable array containing at least @p pixel_count floats.
/// @param pixel_count Number of depth elements to clear; zero is a no-op.
static inline void vgfx3d_rendertarget_fill_depth_max(float *depth, size_t pixel_count) {
    size_t filled = 1u;
    if (!depth || pixel_count == 0)
        return;
    depth[0] = FLT_MAX;
    while (filled < pixel_count) {
        size_t copy_count = filled;
        if (copy_count > pixel_count - filled)
            copy_count = pixel_count - filled;
        memcpy(depth + filled, depth, copy_count * sizeof(float));
        filled += copy_count;
    }
}

/// @brief Lazily allocate the float depth buffer and initialize it to FLT_MAX.
/// @details No-op if already allocated; fails on invalid dims or size overflow.
/// @param target Mutable render target that owns the resulting depth allocation.
/// @return 1 if the depth buffer is available, 0 on failure.
static inline int vgfx3d_rendertarget_ensure_depth(vgfx3d_rendertarget_t *target) {
    size_t pixel_count;
    if (!target)
        return 0;
    if (!vgfx3d_rendertarget_valid_pixels(target, &pixel_count))
        return 0;
    if (pixel_count > SIZE_MAX / sizeof(float))
        return 0;
    if (target->depth_buf)
        return 1;
    target->depth_buf = (float *)malloc(pixel_count * sizeof(float));
    if (!target->depth_buf)
        return 0;
    vgfx3d_rendertarget_fill_depth_max(target->depth_buf, pixel_count);
    return 1;
}

/// @brief Pull the latest GPU/backend color into the CPU buffer if dirty.
/// @details Invokes the registered sync_color callback; clears color_dirty on
///          success.
/// @param target Mutable render target whose CPU mirror is requested.
/// @return One when the color mirror was already current or synchronized
///   successfully; zero for invalid layout or unavailable/failed synchronization.
static inline int vgfx3d_rendertarget_sync_color_if_needed(vgfx3d_rendertarget_t *target) {
    if (!target)
        return 0;
    if (!vgfx3d_rendertarget_valid_color_layout(target, NULL))
        return 0;
    if (!target->color_dirty)
        return 1;
    if (!target->sync_color)
        return 0;
    if (!target->sync_color(target->sync_color_userdata, target))
        return 0;
    target->color_dirty = 0;
    return 1;
}

/// @brief Detach any color-sync callback and clear dirty/HDR-valid flags
///        (used when a target stops being backed by a live GPU surface).
/// @param target Mutable render target to detach; `NULL` is ignored.
static inline void vgfx3d_rendertarget_clear_sync(vgfx3d_rendertarget_t *target) {
    if (!target)
        return;
    target->color_dirty = 0;
    target->hdr_color_valid = 0;
    target->sync_color = NULL;
    target->sync_color_userdata = NULL;
}

/// @brief Detach the backend sync callback while preserving the current dirty bit.
/// @details Used when a backend resource is about to be destroyed or reused after a failed
/// readback.
///   The CPU mirror is no longer trustworthy, but clearing `color_dirty` would make `AsPixels`
///   return stale bytes. Leaving dirty set with no callback makes readback fail explicitly.
/// @param target Mutable render target to detach while retaining its prior dirty state.
static inline void vgfx3d_rendertarget_detach_sync_preserve_dirty(vgfx3d_rendertarget_t *target) {
    int8_t dirty;
    if (!target)
        return;
    dirty = target->color_dirty;
    vgfx3d_rendertarget_clear_sync(target);
    target->color_dirty = dirty;
}

/// @brief Clear a backend-release hook without invoking it.
/// @details Backend cache teardown uses this after it has already removed the corresponding
/// native entry. The color-sync hook is independent and is intentionally left unchanged.
/// @param target Mutable render target whose release callback is cleared.
static inline void vgfx3d_rendertarget_clear_backend_release(vgfx3d_rendertarget_t *target) {
    if (!target)
        return;
    target->release_backend = NULL;
    target->release_backend_userdata = NULL;
}

/// @brief Notify the backend that @p target is about to disappear or change native owners.
/// @details Clears the hook before invoking it so callbacks may safely remove cache entries or
/// re-enter cleanup without a double notification. The target shell remains valid for the entire
/// callback.
/// @param target Mutable render target whose backend owner is notified; `NULL`
///   or a missing callback is a no-op.
static inline void vgfx3d_rendertarget_release_backend(vgfx3d_rendertarget_t *target) {
    vgfx3d_rendertarget_release_fn release_fn;
    void *userdata;
    if (!target || !target->release_backend)
        return;
    release_fn = target->release_backend;
    userdata = target->release_backend_userdata;
    vgfx3d_rendertarget_clear_backend_release(target);
    release_fn(userdata, target);
}

/// @brief RenderTarget3D payload: a GC wrapper holding the backing render target plus
///   its width/height and a lazily-created Pixels mirror for material binding.
typedef struct {
    void *vptr;
    vgfx3d_rendertarget_t *target;
    int64_t width;
    int64_t height;
    void *material_pixels;             /* cached Pixels mirror for RT-as-texture binding */
    uint64_t material_pixels_revision; /* content_revision the mirror was refreshed at */
} rt_rendertarget3d;

/// @brief Resolve a RenderTarget3D handle to its material-binding Pixels mirror,
///   refreshing the mirror only when the target's content changed since the last
///   completed frame into it. Returns NULL for invalid handles.
/// @param obj Borrowed RenderTarget3D runtime handle.
/// @return Borrowed cached Pixels mirror refreshed to the current content
///   revision, or `NULL` when validation, synchronization, or allocation fails.
void *rt_rendertarget3d_material_pixels(void *obj);

/// @brief Canvas3D payload — the central 3D rendering context. Holds the window and
///   selected backend vtable+ctx, per-frame state and the deferred draw-command queues
///   (opaque/transparent/overlay) used for transparency sorting, retained lights/skybox/
///   post-FX chain, fog and shadow-map state, pending terrain-splat inputs, per-frame
///   temp buffers/objects, frame timing plus synthetic input/clock state for deterministic
///   runs, and motion-blur transform history.
typedef struct {
    void *vptr;
    vgfx_window_t gfx_win;      /* underlying vgfx window (owns framebuffer) */
    int32_t width;              /* public/logical coordinate width */
    int32_t height;             /* public/logical coordinate height */
    int32_t framebuffer_width;  /* physical backing-pixel width */
    int32_t framebuffer_height; /* physical backing-pixel height */
    int8_t offscreen;           /* created windowless with an explicit render target */

    /* Backend dispatch */
    const vgfx3d_backend_t *backend;     /* vtable (software, metal, d3d11, opengl) */
    void *backend_ctx;                   /* opaque backend state */
    const char *backend_requested_name;  /* backend selected before runtime fallback */
    int8_t backend_fallback;             /* 1 when Canvas3D fell back to software at creation */
    int8_t force_cpu_skinning;           /* 1 = route all skinned draws through CPU skinning */
    int64_t gpu_skinned_draw_count;      /* lifetime count of draws GPU-skinned via palette */
    int64_t skinning_upload_bytes;       /* lifetime bone-palette bytes handed to the backend */
    const char *backend_fallback_reason; /* empty unless backend_fallback is true */

    /* Frame state */
    int8_t in_frame;                       /* 1 = between Begin/End */
    int8_t frame_is_2d;                    /* 1 = active frame uses orthographic 2D projection */
    int8_t frame_is_view_model;            /* 1 = secondary camera-space pass over a fresh depth
                                              buffer (weapon view models); skips skybox + shadows */
    int64_t last_instanced_fallback_count; /* instances routed through the per-draw
                                              fallback (blend/rebase) this frame */
    int64_t last_instanced_fallback_dropped_count; /* instances skipped after an actual chunked
                                                      fallback queue allocation failure */
    int32_t last_submission_status;      /* sticky RT_CANVAS3D_SUBMISSION_* failure code */
    int64_t submission_failure_count;    /* saturating count since construction/explicit reset */
    int8_t test_fail_next_queue_reserve; /* one-shot CTest injection before queue publication */
    int8_t test_fail_next_mesh_snapshot; /* one-shot CTest injection before snapshot allocation */
    /* Exponential height fog (shares fog_color with distance fog). */
    int8_t height_fog_enabled;
    float height_fog_base;
    float height_fog_falloff;
    float height_fog_density;
    float height_fog_blend;
    float height_fog_sun_color[3];  /* inscattering tint (defaults to white) */
    float height_fog_sun_power;     /* view-sun alignment exponent */
    float height_fog_sun_amount;    /* 0 disables inscattering (default) */
    float cached_vp[16];            /* VP matrix cached in begin_frame for debug drawing */
    float cached_cam_pos[3];        /* camera position cached for sort key computation */
    double cached_world_cam_pos[3]; /* unre-based world camera position for diagnostics/safety */
    float cached_render_cam_pos[3]; /* camera position in backend render space */
    float cached_cam_forward[3];    /* forward vector cached for skybox + ortho shading */
    float cached_cam_near; /* active camera near clip distance, for stable cascade splits */
    float cached_cam_far;  /* active camera far clip distance, for stable cascade splits */
    int8_t cached_cam_is_ortho;
    int8_t camera_relative_upload;
    double camera_relative_origin[3];
    float last_scene_vp[16]; /* most recent 3D VP matrix (preserved across 2D passes) */
    float last_scene_cam_pos[3];
    int8_t has_last_scene_vp;

    /* Deferred draw command queue (for transparency sorting) */
    void *draw_cmds; /* dynamic array of deferred_draw_t */
    int32_t draw_count;
    int32_t draw_capacity;
    void *trans_cmds; /* reusable transparent draw scratch buffer */
    int32_t trans_capacity;
    void *sort_cmds; /* reusable deferred stable-sort scratch buffer */
    int32_t sort_capacity;
    uint64_t *sort_keys;         /* cached radix keys, evaluated once per draw per sort */
    uint64_t *sort_keys_scratch; /* scatter companion for sort_keys */
    int32_t sort_key_capacity;
    void *final_overlay_cmds; /* dynamic array of deferred_draw_t, replayed after post-FX */
    int32_t final_overlay_count;
    int32_t final_overlay_capacity;
    void **final_overlay_temp_buffers;
    int32_t final_overlay_temp_buf_count;
    int32_t final_overlay_temp_buf_capacity;
    void **final_overlay_temp_objects; /* GC objs (materials/textures) kept alive until overlay
                                          replay */
    int32_t final_overlay_temp_obj_count;
    int32_t final_overlay_temp_obj_capacity;
    uint8_t *final_overlay_arena; /* Stable vertex/index arena for common final-overlay draws */
    size_t final_overlay_arena_capacity;
    size_t final_overlay_arena_used;
    size_t final_overlay_arena_peak;

    /* Per-frame bump arena for draw-path transients (CPU-skinned vertex
     * buffers, gathered bone palettes). Chunked so growth NEVER relocates —
     * recorded draw commands hold pointers into it until frame flush. One
     * reset per frame replaces what used to be a malloc + dedup-set insert +
     * free per skinned draw. */
    struct canvas3d_frame_arena_chunk *frame_arena_head;
    struct canvas3d_frame_arena_chunk *frame_arena_current;
    size_t frame_arena_frame_bytes; /* bytes handed out this frame */

/* Per-pass CPU milliseconds for the LAST flushed frame (diagnostics; never
 * read by simulation, so VM==native determinism is unaffected). */
#define RT_CANVAS3D_PASS_SHADOW 0
#define RT_CANVAS3D_PASS_MAIN 1
#define RT_CANVAS3D_PASS_OVERLAY 2
#define RT_CANVAS3D_PASS_BACKEND_END 3
#define RT_CANVAS3D_PASS_COUNT 4
    double pass_cpu_ms[RT_CANVAS3D_PASS_COUNT];
    int8_t final_overlay_recording;
    int8_t frame_finalized;
    int8_t frame_presented_by_finalize;

    /* Render target (NULL = render to window) */
    vgfx3d_rendertarget_t *render_target;
    rt_rendertarget3d *render_target_owner; /* retained wrapper for active target */
    uint8_t *readback_rgba_scratch;         /* reusable GPU screenshot staging bytes */
    size_t readback_rgba_scratch_capacity;

    /* Lighting */
    rt_light3d *lights[VGFX3D_MAX_LIGHTS];
    rt_light3d *scene_lights[VGFX3D_MAX_LIGHTS];
    rt_light3d scene_light_storage[VGFX3D_MAX_LIGHTS];
    int32_t scene_light_count; /* transient, not retained: populated by Scene3D.Draw */
    float ambient[3];

    /* Image-based lighting: when enabled and the skybox has a prepared IBL
     * payload, PBR draws without an explicit material env map light their
     * ambient term from SH irradiance + the prefiltered specular chain. */
    int8_t ibl_enabled;
    float ibl_intensity;

    /* Light-snapshot revisioning: queued draws are stamped with a monotonic
     * revision; the stamp only advances when the flattened light set or
     * ambient color actually changed since the previous queued draw, letting
     * backends skip re-uploading scene/light constants for runs of draws. */
    uint32_t lights_revision;
    void *last_light_snapshot; /* vgfx3d_light_params_t[VGFX3D_MAX_LIGHTS], lazily allocated */
    float last_light_snapshot_ambient[3];
    int32_t last_light_snapshot_count;
    int8_t last_light_snapshot_valid;
    void *frame_light_snapshots; /* vgfx3d_light_params_t compact draw-light arena */
    int32_t frame_light_snapshot_count;
    int32_t frame_light_snapshot_capacity;

    /* Over-budget light selection hysteresis: identities of the local lights chosen
     * by the previous flatten. Incumbents get a score boost so near-ties do not swap
     * membership frame-to-frame (which would pop lighting across the whole scene). */
    uintptr_t selected_light_ids[VGFX3D_MAX_LIGHTS];
    int32_t selected_light_id_count;

    /* Per-frame flattened-light cache: draws sharing one light generation reuse a
     * single snapshot slice, revision stamp, and froxel table instead of paying
     * build_light_params + a full snapshot memcmp per queued draw. Invalidated at
     * frame begin, on slot/ambient/scene-light changes, and whenever the global
     * Light3D mutation generation advances (rt_light3d_mutation_revision). */
    int8_t frame_light_cache_valid;
    uint64_t frame_light_cache_revision; /* Light3D mutation generation at flatten */
    int32_t frame_light_cache_limit;     /* active light limit at flatten */
    int32_t frame_light_cache_count;
    int32_t frame_light_cache_offset; /* arena element offset, -1 = no lights */
    uint32_t frame_light_cache_stamp; /* lights_revision stamp for the slice */

    /* Skybox */
    rt_cubemap3d *skybox; /* CubeMap3D for background (or NULL) */
    /* Once-per-End latch: GPU-backend skybox is drawn inside the main pass after
     * opaques (early-Z rejects covered sky pixels); End() keeps a catch-all call for
     * frames whose main pass ran empty. */
    int8_t skybox_pass_done;
    uint8_t *skybox_cpu_cache; /* tightly packed RGBA8 fallback skybox */
    int32_t skybox_cpu_cache_w;
    int32_t skybox_cpu_cache_h;
    uint64_t skybox_cpu_cache_generation;
    int8_t skybox_cpu_cache_is_ortho;
    float skybox_cpu_cache_vp[16];
    float skybox_cpu_cache_cam_pos[3];
    float skybox_cpu_cache_forward[3];

    /* Post-processing effect chain (NULL = disabled) */
    void *postfx;
    vgfx3d_postfx_chain_t frame_postfx_chain;
    int8_t frame_gpu_postfx_enabled;
    int8_t frame_postfx_state_latched;
    int32_t quality_requested;
    int32_t quality_active;
    int32_t quality_fallback_reason;
    int8_t quality_fallback;

    /* Temporary raw buffers freed at end of frame (e.g., skinned vertex data) */
    void **temp_buffers;
    int32_t temp_buf_count;
    int32_t temp_buf_capacity;
    void **temp_buffer_set;
    int32_t temp_buffer_set_capacity;
    rt_canvas3d_float_snapshot_entry *float_snapshots;
    int32_t float_snapshot_count;
    int32_t float_snapshot_capacity;
    rt_canvas3d_mesh_snapshot_entry *mesh_snapshots;
    int32_t mesh_snapshot_count;
    int32_t mesh_snapshot_capacity;
    int32_t *mesh_snapshot_hash;
    int32_t mesh_snapshot_hash_capacity;
    /* 0 when the hash table indexes every snapshot (the common case); set to 1 if a
     * hash rebuild fails (OOM) so lookups fall back to the linear scan for safety. */
    int8_t mesh_snapshot_hash_dirty;
    size_t mesh_snapshot_bytes;
    int64_t last_mesh_snapshot_bytes;         /* snapshot bytes copied by the latest ended frame */
    int64_t last_mesh_snapshot_drop_count;    /* snapshot allocations/budget denials this frame */
    int64_t last_mesh_snapshot_dropped_bytes; /* requested snapshot bytes denied this frame */
    vgfx3d_skinning_scratch_t skinning_scratch;

    /* Temporary runtime objects retained until end of frame */
    void **temp_objects;
    int32_t temp_obj_count;
    int32_t temp_obj_capacity;
    void **temp_object_set;
    int32_t temp_object_set_capacity;

    /* Reusable text rendering scratch buffers */
    vgfx3d_vertex_t *text_vertices;
    int32_t text_vertex_capacity;
    uint32_t *text_indices;
    int32_t text_index_capacity;
    rt_canvas3d_aa_text_cache_entry *aa_text_cache;
    int32_t aa_text_cache_count;
    size_t aa_text_cache_bytes;

    /* Distance fog */
    int8_t fog_enabled;
    float fog_near;
    float fog_far;
    float fog_color[3];

    /* Present pacing: requested vsync state (default on); applied through the
     * backend set_vsync hook when available. GPU backends own their pacing;
     * software_frame_limit preserves the vgfx creation cap for the CPU backend. */
    int8_t vsync_enabled;
    int32_t software_frame_limit;

    /* Requested scene render scale (1 = native); applied through the backend
     * set_render_scale hook when supported. */
    float render_scale;

    /* CPU occlusion Hi-Z: fine per-texel view-depth buffer written by the software
     * occluder rasterizer (lazily allocated while occlusion culling is enabled). The
     * coarse grid keeps the conservative max over each fine block, so the coverage
     * test is unchanged while writes gain true triangle silhouettes instead of AABB
     * rectangles. Over-budget draws rasterize an exact bounded subset of source triangles;
     * omitted geometry reduces culling but cannot invent coverage. The vertex scratch holds one
     * precise-path draw's projected vertices. */
    float *hiz_depth; /* CANVAS3D_HIZ_W * CANVAS3D_HIZ_H floats */
    float *hiz_vertex_scratch;
    int32_t hiz_vertex_scratch_capacity; /* in vertices (4 floats each) */
    int32_t hiz_frame_triangles;         /* rasterized this frame, budget-capped */
    int32_t hiz_frame_proxy_triangles;   /* exact subset triangles after precise-budget overflow */

    /* Shadow mapping */
    int8_t shadows_enabled;
    int32_t shadow_resolution;
    float shadow_bias;
    int32_t shadow_count;
    int32_t shadow_cascade_count;
    float shadow_slope_bias;
    /* Maximum camera distance covered by directional shadow fitting. 0 = auto
     * (min(camera far, 300)). Capping the cascade range instead of spanning the whole
     * clip range concentrates shadow-map texels near the camera; without it a 5000-unit
     * far plane collapses near-shadow resolution into blocky, shimmering blobs. */
    float shadow_distance;
    /* Shadow-caster sweep: the primary shadow-casting directional light's travel
     * direction scaled by the effective shadow distance. Scene traversal extends node
     * AABBs by this vector before frustum tests so casters just outside the view
     * frustum still enqueue — without it their shadows pop in/out at the screen edge
     * as the camera turns. Zero/inactive when shadows are off or no caster exists. */
    float shadow_caster_sweep[3];
    int8_t shadow_caster_sweep_active;
    /* Plan 06: occlusion darkening (0..1; 0.85 reproduces the legacy 0.15 lit floor)
     * and PCF tier (0 = 4 taps, 1 = 8, 2 = 16 rotated-Poisson taps). */
    float shadow_strength;
    int32_t shadow_quality;
    vgfx3d_rendertarget_t *shadow_rts[VGFX3D_MAX_SHADOW_LIGHTS];
    int8_t shadow_rt_owned[VGFX3D_MAX_SHADOW_LIGHTS]; /* slots allocated by Canvas3D itself */
    float shadow_light_vps[VGFX3D_MAX_SHADOW_LIGHTS][16];
    /* Per-slot shadow content signature (light VP + resolution + bias + every
     * culled-in caster's geometry identity/revision/transform). When a slot's
     * signature matches last frame's and the backend can re-arm its stored
     * depth (shadow_reuse hook), the whole begin/draw/end pass is skipped —
     * fully static shadow maps stop re-rendering every frame. 0 = never reuse
     * (animated caster present, or the slot was not rendered). */
    uint64_t shadow_slot_signature[VGFX3D_MAX_SHADOW_LIGHTS];
    int64_t last_shadow_slots_cached; /* diagnostics: slots reused this frame */

    /* Auto-instancing scratch: consecutive state-sorted opaque draws that share
     * identical commands (same stable geometry, material, lights — everything but
     * the model transform) are folded into one instanced submission. */
    float *auto_instance_matrices;
    int32_t auto_instance_matrix_capacity;
    int64_t last_auto_instanced_draws; /* diagnostics: draws folded into batches */

    /* Plan 08: overlay 2D clip rect (enqueue-time CPU clipping — applies to
     * screen-space rect/line/image/text queueing while active; backend-neutral). */
    int8_t overlay_clip_active;
    float overlay_clip_x;
    float overlay_clip_y;
    float overlay_clip_w;
    float overlay_clip_h;

    /* Pending terrain splat data (consumed by next draw_mesh call, then cleared) */
    int8_t pending_has_splat;
    const void *pending_splat_map;
    const void *pending_splat_layers[4];
    float pending_splat_layer_scales[4];

    /* Rendering options */
    int8_t wireframe;
    int8_t backface_cull;
    int8_t frustum_culling;
    int8_t occlusion_culling;
    int8_t opaque_depth_sorting;
    float occlusion_depth_margin;
    int32_t occlusion_rect_expand_cells;
    int8_t clustered_lighting;
    /* Plan 07: revision-keyed froxel table ring (vgfx3d_cluster_table_t[count],
     * lazily allocated; entries are invalidated at frame Begin because binning
     * is camera-dependent). */
    void *cluster_tables;
    int32_t cluster_table_count;
    int32_t cluster_table_cursor;
    int64_t cluster_overflow_total;   /* lifetime truncated cluster entries (diagnostics) */
    int32_t cluster_light_budget;     /* per-cluster light-index capacity (8..64) */
    int32_t last_dropped_light_count; /* forward-path lights truncated by the active limit */
    int32_t shadow_budget;            /* general shadow-light slots (1..VGFX3D_MAX_SHADOW_LIGHTS) */
    int32_t last_shadow_slots_used; /* shadow slots rendered in the latest frame (incl. cascades) */
    int32_t last_shadow_requests_dropped; /* shadow-requesting lights denied a slot this frame */
    void *shadow_draw_indices;            /* int32_t scratch list of shadow-casting draw indices */
    int32_t shadow_draw_index_capacity;
    int32_t last_draw_count;
    int32_t last_occluded_draw_count;
    int32_t last_frustum_culled_draw_count;
    int32_t last_cpu_occluded_draw_count;
    int32_t last_occlusion_candidate_count;
    int64_t last_texture_upload_bytes;
    int64_t last_frame_gpu_time_us;
    /* Per-pass draw attribution (plan 30): Shadow/Opaque/Transparent/PostFX/
     * Overlay/Present. CPU-exact tallies bumped at the submit seams. */
    int64_t last_pass_draw_count[6];
    int64_t last_pass_instance_count[6];

    /* Automatic texture mip-residency streaming (opt-in; default off). The
     * table tracks per-TextureAsset3D screen-space texel demand aggregated per
     * frame; decisions move each asset's resident mip window through
     * rt_textureasset3d_set_resident_mip_range at the asset's first draw-time
     * touch of a frame, before any draw command snapshots the window. Entries
     * hold identities and counters only — never object references. */
    void *texture_stream_entries; /* canvas3d_texture_stream_entry_t array */
    int32_t texture_stream_capacity;
    int32_t texture_stream_live;
    int8_t texture_stream_enabled;
    double texture_stream_bias;       /* added to the desired mip; >0 drops more detail */
    int64_t texture_stream_demotions; /* lifetime applied window demotions (diagnostics) */
    int64_t frame_draws_submitted;
    int64_t frame_aabb_transforms;
    int64_t frame_sort_passes;
    int64_t frame_backend_state_changes;
    uint64_t frame_last_backend_state_key;
    int8_t frame_has_backend_state_key;
    rt_canvas3d_world_bounds_cache_entry world_bounds_cache[RT_CANVAS3D_WORLD_BOUNDS_CACHE_SIZE];

    /* Timing */
    int64_t frame_serial;
    int64_t last_flip_us;
    int64_t delta_time_us;
    int64_t delta_time_ms;
    int64_t fps_sample_us[32];
    int64_t fps_sample_total_us;
    int32_t fps_sample_index;
    int32_t fps_sample_count;
    int64_t dt_max_ms;
    int64_t timing_serial;
    int8_t frame_timing_updated_by_poll;
    int32_t input_source;
    int32_t clock_source;
    int64_t synthetic_dt_us;
    int64_t synthetic_key_keys[64];
    int8_t synthetic_key_downs[64];
    int32_t synthetic_key_count;
    uint8_t synthetic_key_state[ZANNA_KEY_MAX];
    double synthetic_mouse_dx;
    double synthetic_mouse_dy;
    double synthetic_mouse_wheel_y;
    int64_t synthetic_mouse_buttons;
    int8_t synthetic_mouse_has_buttons;
    uint8_t synthetic_mouse_button_state[ZANNA_MOUSE_BUTTON_MAX];
    /* Relative (raw) mouse mode applied to the platform window; reconciled
     * against rt_mouse_get_relative_mode() each poll so the runtime input
     * layer stays window-handle-free. */
    int8_t relative_mouse_applied;
    int8_t should_close;
    int64_t last_event_type;
    int64_t event_type_queue[RT_CANVAS3D_EVENT_QUEUE_CAPACITY];
    int32_t event_type_head;
    int32_t event_type_count;
    int64_t event_type_dropped_count; /* lifetime window/input events dropped from the ring */

    /* Previous-frame transform history for motion blur */
    void *motion_history;
    int32_t motion_history_count;
    int32_t motion_history_capacity;
    int32_t *motion_history_hash;
    int32_t motion_history_hash_capacity;
    int32_t motion_history_retention_frames;
    rt_canvas3d_occlusion_history_entry *occlusion_history;
    int32_t occlusion_history_count;
    int32_t occlusion_history_capacity;
    int32_t *occlusion_history_hash;
    int32_t occlusion_history_hash_capacity;
    rt_canvas3d_occlusion_duplicate_entry *occlusion_duplicate_counts;
    int32_t occlusion_duplicate_count_capacity;
    int8_t occlusion_state_valid;
    /* Keyed by the target's monotonic cache_identity (0 = window/default output),
     * not the pointer: a freed target reallocated at the same address must not
     * inherit covered-streak occlusion history (ABA). */
    uint64_t occlusion_last_render_target_identity;
    int32_t occlusion_last_output_width;
    int32_t occlusion_last_output_height;
    double occlusion_last_world_cam_pos[3];
    float occlusion_last_cam_forward[3];
    float occlusion_last_near;
    float occlusion_last_far;
    int8_t occlusion_last_is_ortho;
} rt_canvas3d;

/// @brief Validate a Canvas3D handle while optionally preserving internal stack fixtures.
/// @details Production handles must carry the Canvas3D class id. Backend/unit tests
///   may opt into plain stack rt_canvas3d fixtures by compiling graphics runtime code
///   with `RT_G3D_ALLOW_STACK_FIXTURES=1`; production builds should leave it unset so
///   arbitrary non-heap pointers are rejected before any Canvas3D fields are read.
/// @param obj Borrowed candidate Canvas3D payload.
/// @return Validated mutable Canvas3D pointer, an allowed stack fixture, or `NULL`.
static inline rt_canvas3d *rt_canvas3d_checked_or_stack(void *obj) {
    rt_canvas3d *c = (rt_canvas3d *)rt_g3d_checked_or_null(obj, RT_G3D_CANVAS3D_CLASS_ID);
    if (c)
        return c;
#if defined(RT_G3D_ALLOW_STACK_FIXTURES) && RT_G3D_ALLOW_STACK_FIXTURES
    if (obj && !rt_heap_is_payload(obj))
        return (rt_canvas3d *)obj;
#endif
    return NULL;
}

/// @brief Validate a Camera3D handle while optionally preserving internal stack fixtures.
/// @param obj Borrowed candidate Camera3D payload.
/// @return Validated mutable Camera3D pointer, an allowed stack fixture, or `NULL`.
static inline rt_camera3d *rt_camera3d_checked_or_stack(void *obj) {
    rt_camera3d *cam = (rt_camera3d *)rt_g3d_checked_or_null(obj, RT_G3D_CAMERA3D_CLASS_ID);
    if (cam)
        return cam;
#if defined(RT_G3D_ALLOW_STACK_FIXTURES) && RT_G3D_ALLOW_STACK_FIXTURES
    if (obj && !rt_heap_is_payload(obj))
        return (rt_camera3d *)obj;
#endif
    return NULL;
}

//=============================================================================
// Mat4 internal access (matches rt_mat4.c layout)
//=============================================================================

/// @brief Internal Mat4 layout (row-major m[r*4+c]) matching rt_mat4.c, used to read a
///   Mat4 object's matrix without going through the public accessor API.
typedef struct {
    double m[16]; /* row-major: m[r*4+c] */
} mat4_impl;

/// @brief Internal: submit a mesh draw with an explicit row-major Mat4 (skips Mat4 wrapper alloc).
/// @param obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live Mesh3D to queue.
/// @param model_matrix Borrowed array of 16 row-major doubles captured by submission.
/// @param material_obj Borrowed live Material3D applied to the draw.
void rt_canvas3d_draw_mesh_matrix(void *obj,
                                  void *mesh_obj,
                                  const double *model_matrix,
                                  void *material_obj);
/// @brief Internal: submit a mesh draw with motion-key + previous bone/morph data for TAA + motion
/// blur.
/// @param obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live Mesh3D to queue.
/// @param model_matrix Borrowed array of 16 row-major doubles.
/// @param material_obj Borrowed live Material3D applied to the draw.
/// @param motion_key Optional stable instance identity for temporal history.
/// @param prev_bone_palette Optional borrowed previous-frame bone matrices.
/// @param prev_morph_weights Optional borrowed previous-frame morph weights.
void rt_canvas3d_draw_mesh_matrix_keyed(void *obj,
                                        void *mesh_obj,
                                        const double *model_matrix,
                                        void *material_obj,
                                        const void *motion_key,
                                        const float *prev_bone_palette,
                                        const float *prev_morph_weights);
/// @brief Internal: submit a mesh draw with explicit local bounds and occlusion safety metadata.
/// @details The explicit bounds path is used by Scene3D/extras that already computed conservative
///   local bounds for dynamic deformation or chunking. `conservative_bounds` preserves frustum
///   culling while disabling exact-coverage assumptions in CPU occlusion. `disable_occlusion`
///   skips both CPU occlusion testing and occluder writes for draws whose coverage is not a solid
///   projection of their AABB. `culling_pad` expands only CPU frustum tests, which is useful for
///   terrain chunks whose edge triangles must survive small CPU/GPU precision differences.
/// @param obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live Mesh3D to queue.
/// @param model_matrix Borrowed array of 16 row-major doubles.
/// @param material_obj Borrowed live Material3D applied to the draw.
/// @param motion_key Optional stable identity for temporal history and ordering.
/// @param prev_bone_palette Optional borrowed previous-frame bone matrices.
/// @param prev_morph_weights Optional borrowed previous-frame morph weights.
/// @param local_bounds_min Optional three-component local AABB minimum.
/// @param local_bounds_max Optional three-component local AABB maximum paired
///   with @p local_bounds_min.
/// @param conservative_bounds Non-zero when the supplied bounds conservatively
///   enclose dynamic geometry.
/// @param disable_occlusion Non-zero to skip occlusion testing and writes.
/// @param culling_pad Non-negative world-space frustum-culling expansion.
void rt_canvas3d_draw_mesh_matrix_keyed_bounds(void *obj,
                                               void *mesh_obj,
                                               const double *model_matrix,
                                               void *material_obj,
                                               const void *motion_key,
                                               const float *prev_bone_palette,
                                               const float *prev_morph_weights,
                                               const float *local_bounds_min,
                                               const float *local_bounds_max,
                                               int8_t conservative_bounds,
                                               int8_t disable_occlusion,
                                               float culling_pad);
/// @brief Internal: enable camera-relative float upload for large-world Game3D frames.
/// @param obj Borrowed Canvas3D handle.
/// @param enabled Non-zero to rebase uploads around the active camera; zero for
///   absolute float coordinates.
void rt_canvas3d_set_camera_relative_upload(void *obj, int8_t enabled);
/// @brief Internal: copy the active camera-relative origin; returns 1 when upload rebasing is on.
/// @param obj Borrowed Canvas3D handle.
/// @param out_origin Non-`NULL` three-double output receiving the current origin.
/// @return One when rebasing is enabled and the origin was copied; otherwise zero.
int rt_canvas3d_get_camera_relative_origin(void *obj, double out_origin[3]);
/// @brief Internal: invalidate and release the cached CPU skybox fallback image.
/// @param canvas Mutable Canvas3D owning the cache; `NULL` is ignored.
void rt_canvas3d_invalidate_skybox_cache(rt_canvas3d *canvas);
/// @brief Internal: submit a mesh draw after applying morph targets.
/// @param canvas Borrowed Canvas3D handle with an active frame.
/// @param mesh Borrowed live Mesh3D to deform and submit.
/// @param model_matrix Borrowed array of 16 row-major doubles.
/// @param material Borrowed live Material3D applied to the draw.
/// @param motion_key Optional stable instance identity for temporal history.
/// @param morph_targets Borrowed MorphTarget3D supplying deltas and weights.
void rt_canvas3d_draw_mesh_matrix_morphed(void *canvas,
                                          void *mesh,
                                          const double *model_matrix,
                                          void *material,
                                          const void *motion_key,
                                          void *morph_targets);
/// @brief Internal: submit a morph-target draw while preserving explicit conservative bounds.
/// @details Scene3D precomputes expanded local bounds for animated/morphed geometry. This variant
///          carries those bounds plus occlusion safety flags through the attached-MorphTarget fast
///          path so morphed vertices are not culled or CPU-occluded against the static mesh AABB.
/// @param canvas Borrowed Canvas3D handle with an active frame.
/// @param mesh Borrowed live Mesh3D to deform and submit.
/// @param model_matrix Borrowed array of 16 row-major doubles.
/// @param material Borrowed live Material3D applied to the draw.
/// @param motion_key Optional stable instance identity for temporal history.
/// @param morph_targets Borrowed MorphTarget3D supplying deltas and weights.
/// @param local_bounds_min Optional three-component conservative local AABB minimum.
/// @param local_bounds_max Optional three-component local AABB maximum paired
///   with @p local_bounds_min.
/// @param conservative_bounds Non-zero when supplied bounds include all deformation.
/// @param disable_occlusion Non-zero to skip occlusion testing and writes.
void rt_canvas3d_draw_mesh_matrix_morphed_bounds(void *canvas,
                                                 void *mesh,
                                                 const double *model_matrix,
                                                 void *material,
                                                 const void *motion_key,
                                                 void *morph_targets,
                                                 const float *local_bounds_min,
                                                 const float *local_bounds_max,
                                                 int8_t conservative_bounds,
                                                 int8_t disable_occlusion);
/// @brief Internal: retain a GC-managed object until the current frame is fully submitted.
/// @param obj Borrowed Canvas3D handle that will own the temporary reference.
/// @param value Borrowed GC-managed object to retain once.
/// @return Non-zero when the object is already tracked or retained successfully.
int rt_canvas3d_add_temp_object(void *obj, void *value);
/// @brief Internal: sample a cubemap direction into linear RGB components.
/// @param cm Borrowed complete CubeMap3D payload.
/// @param dx Direction X component.
/// @param dy Direction Y component.
/// @param dz Direction Z component.
/// @param out_r Non-`NULL` red-channel output.
/// @param out_g Non-`NULL` green-channel output.
/// @param out_b Non-`NULL` blue-channel output.
void rt_cubemap_sample(
    const rt_cubemap3d *cm, float dx, float dy, float dz, float *out_r, float *out_g, float *out_b);
/// @brief Internal: sample a cubemap with an already normalized finite direction.
/// @details This avoids the sampler's defensive normalization in CPU hot paths that already
///          sanitized the direction, such as the Canvas3D software skybox cache.
/// @param cm Borrowed complete CubeMap3D payload.
/// @param dx Normalized finite direction X component.
/// @param dy Normalized finite direction Y component.
/// @param dz Normalized finite direction Z component.
/// @param out_r Non-`NULL` red-channel output.
/// @param out_g Non-`NULL` green-channel output.
/// @param out_b Non-`NULL` blue-channel output.
void rt_cubemap_sample_unit(
    const rt_cubemap3d *cm, float dx, float dy, float dz, float *out_r, float *out_g, float *out_b);
/// @brief Internal: sample a cubemap reflection with a roughness-dependent blur kernel.
/// @param cm Borrowed complete CubeMap3D payload.
/// @param dx Reflection direction X component.
/// @param dy Reflection direction Y component.
/// @param dz Reflection direction Z component.
/// @param roughness Normalized surface roughness controlling blur width.
/// @param out_r Non-`NULL` red-channel output.
/// @param out_g Non-`NULL` green-channel output.
/// @param out_b Non-`NULL` blue-channel output.
void rt_cubemap_sample_roughness(const rt_cubemap3d *cm,
                                 float dx,
                                 float dy,
                                 float dz,
                                 float roughness,
                                 float *out_r,
                                 float *out_g,
                                 float *out_b);
/// @brief Internal: lazily compute the cubemap's IBL payload (SH-9 irradiance +
///   GGX-prefiltered specular mip chain). Idempotent; returns 1 when ready.
/// @param cubemap Borrowed live complete CubeMap3D to prepare.
/// @return One when the IBL payload is already or newly ready; zero on invalid
///   input or allocation/precomputation failure.
int rt_cubemap3d_ensure_ibl(void *cubemap);
/// @brief Internal: sample the prefiltered specular chain (trilinear across
///   roughness levels). Falls back to rt_cubemap_sample_roughness when the IBL
///   payload has not been prepared.
/// @param cm Borrowed CubeMap3D payload.
/// @param dx Reflection direction X component.
/// @param dy Reflection direction Y component.
/// @param dz Reflection direction Z component.
/// @param roughness Normalized roughness selecting and blending prefiltered levels.
/// @param out_r Non-`NULL` red-channel output.
/// @param out_g Non-`NULL` green-channel output.
/// @param out_b Non-`NULL` blue-channel output.
void rt_cubemap_sample_ibl(const rt_cubemap3d *cm,
                           float dx,
                           float dy,
                           float dz,
                           float roughness,
                           float *out_r,
                           float *out_g,
                           float *out_b);
/// @brief Internal: evaluate SH-9 irradiance coefficients (as stored in
///   rt_cubemap3d.ibl_sh) along a unit normal. Output is linear RGB with the
///   Lambertian 1/pi already folded in (a constant environment of color C
///   evaluates to C for every normal).
/// @param sh Borrowed array of 27 RGB-interleaved SH-9 coefficients.
/// @param nx Unit surface-normal X component.
/// @param ny Unit surface-normal Y component.
/// @param nz Unit surface-normal Z component.
/// @param out_rgb Non-`NULL` three-float linear RGB output.
void rt_sh9_eval_irradiance(const float sh[27], float nx, float ny, float nz, float *out_rgb);
/// @brief Internal: apply a canvas's active post-processing chain.
/// @param canvas Borrowed Canvas3D handle whose completed frame is processed.
void rt_postfx3d_apply_to_canvas(void *canvas);
/// @brief Internal: inject a mouse delta without changing absolute position.
/// @param dx Horizontal relative-motion delta in input units.
/// @param dy Vertical relative-motion delta in input units.
void rt_mouse_force_delta(int64_t dx, int64_t dy);
/// @brief Internal: queue an instanced batch (one draw call rendering many transforms of @p mesh).
/// @param canvas_obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared geometry.
/// @param material_obj Borrowed live Material3D shared by all instances.
/// @param instance_matrices Borrowed current row-major matrix array.
/// @param instance_count Number of matrices and instances.
/// @param prev_instance_matrices Optional borrowed previous-frame matrix array.
/// @param has_prev_instance_matrices Non-zero when the previous array is present.
void rt_canvas3d_queue_instanced_batch(void *canvas_obj,
                                       void *mesh_obj,
                                       void *material_obj,
                                       const float *instance_matrices,
                                       int32_t instance_count,
                                       const float *prev_instance_matrices,
                                       int8_t has_prev_instance_matrices);
/// @brief Internal: queue an instanced batch whose matrices are already frame-relative.
/// @param canvas_obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared geometry.
/// @param material_obj Borrowed live Material3D shared by all instances.
/// @param instance_matrices Borrowed current matrices already in frame render space.
/// @param instance_count Number of matrices and instances.
/// @param prev_instance_matrices Optional borrowed prior matrices in the same space.
/// @param has_prev_instance_matrices Non-zero when the previous array is present.
void rt_canvas3d_queue_instanced_batch_frame_matrices(void *canvas_obj,
                                                      void *mesh_obj,
                                                      void *material_obj,
                                                      const float *instance_matrices,
                                                      int32_t instance_count,
                                                      const float *prev_instance_matrices,
                                                      int8_t has_prev_instance_matrices);
/// @brief Internal: queue an instanced batch with explicit local bounds and occlusion flags.
/// @param canvas_obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live Mesh3D used as shared geometry.
/// @param material_obj Borrowed live Material3D shared by all instances.
/// @param instance_matrices Borrowed current row-major matrix array.
/// @param instance_count Number of matrices and instances.
/// @param prev_instance_matrices Optional borrowed previous-frame matrix array.
/// @param has_prev_instance_matrices Non-zero when the previous array is present.
/// @param local_bounds_min Optional three-component local AABB minimum.
/// @param local_bounds_max Optional three-component local AABB maximum paired
///   with @p local_bounds_min.
/// @param conservative_bounds Non-zero when the supplied bounds are conservative.
/// @param disable_occlusion Non-zero to disable batch occlusion.
void rt_canvas3d_queue_instanced_batch_bounds(void *canvas_obj,
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

/// @brief Report whether the active Canvas3D backend accepts compact particle instances.
/// @details Returns false for software and for a partially initialized GPU backend, allowing the
///   caller to retain the deterministic CPU-expanded path without relying on backend-name tests.
/// @param canvas_obj Canvas3D runtime handle or approved stack fixture.
/// @return Non-zero only when the backend advertises particle instancing and has an instanced draw
///   hook.
int rt_canvas3d_supports_particle_instancing(void *canvas_obj);

/// @brief Queue a sorted compact particle batch against the renderer's retained unit quad.
/// @details The function snapshots the instance records into frame-owned storage, snapshots and
///   retains the material dependencies through the ordinary deferred queue, computes conservative
///   aggregate bounds, and disables batch-level occlusion because one aggregate rectangle cannot
///   safely represent separated particles. The supplied order is preserved exactly, which is
///   required for alpha-blended back-to-front rendering.
/// @param canvas_obj Canvas3D runtime handle or approved stack fixture.
/// @param material_obj Configured Material3D used by every particle in the batch.
/// @param instances Finite camera-relative compact records in final draw order.
/// @param instance_count Number of records; must be in `[1, CANVAS3D_MAX_INSTANCES]`.
/// @return Non-zero when one deferred hardware command was queued; zero when unsupported or when
///   validation/allocation prevents submission.
int rt_canvas3d_queue_particle_batch(void *canvas_obj,
                                     void *material_obj,
                                     const vgfx3d_particle_instance_t *instances,
                                     int32_t instance_count);
/// @brief Internal: queue one per-instance-skinned hardware-instanced chunk (R18).
/// @details bone_palettes packs instance_count consecutive palettes of instance_bone_stride
///   bones each (total_bone_count matrices, <= VGFX3D_MAX_BONES); storage must outlive the
///   frame flush. Returns 1 when queued, 0 when the draw cannot take the hardware path and
///   the caller must fall back to individual skinned draws.
/// @param canvas_obj Borrowed Canvas3D handle with an active frame.
/// @param mesh_obj Borrowed live skinned Mesh3D.
/// @param material_obj Borrowed live Material3D shared by all instances.
/// @param instance_matrices Borrowed current row-major instance matrices.
/// @param instance_count Number of instances in the chunk.
/// @param bone_palettes Borrowed packed current-pose matrices grouped by instance.
/// @param prev_bone_palettes Optional borrowed packed previous-pose matrices.
/// @param total_bone_count Total number of matrices in @p bone_palettes.
/// @param instance_bone_stride Number of bone matrices per instance.
/// @return One when queued; zero when hardware instancing cannot represent the palettes.
int rt_canvas3d_queue_instanced_batch_skinned(void *canvas_obj,
                                              void *mesh_obj,
                                              void *material_obj,
                                              const float *instance_matrices,
                                              int32_t instance_count,
                                              const float *bone_palettes,
                                              const float *prev_bone_palettes,
                                              int32_t total_bone_count,
                                              int32_t instance_bone_stride);
/// @brief Internal: get the monotonic per-frame counter (used to seed motion-blur history).
/// @param obj Borrowed Canvas3D handle.
/// @return Positive current frame serial, or zero for invalid/unstarted canvases.
int64_t rt_canvas3d_get_frame_serial(void *obj);
/// @brief Internal: begin a HUD/overlay sub-pass; @p preserve_existing_color skips the initial
/// clear.
/// @param c Borrowed Canvas3D prepared to begin an overlay pass.
/// @param preserve_existing_color Non-zero to load existing scene color.
/// @return Non-zero when the overlay frame began successfully.
int canvas3d_begin_overlay_frame(rt_canvas3d *c, int8_t preserve_existing_color);
/// @brief Internal: borrow the most recently used scene VP matrix for billboard alignment.
/// @param c Borrowed Canvas3D to inspect.
/// @return Borrowed 16-float view-projection matrix, or `NULL` when unavailable.
const float *canvas3d_active_scene_vp(const rt_canvas3d *c);
/// @brief Internal: queue a 2D rect into the overlay pass at clip-space position with RGBA color.
/// @param c Borrowed Canvas3D receiving the overlay command.
/// @param x Left edge in screen pixels.
/// @param y Top edge in screen pixels.
/// @param w Width in screen pixels.
/// @param h Height in screen pixels.
/// @param r Normalized red component.
/// @param g Normalized green component.
/// @param b Normalized blue component.
/// @param a Normalized alpha component.
/// @return Non-zero when visible geometry is queued; zero on invalid or clipped input.
int canvas3d_queue_screen_rect(
    rt_canvas3d *c, float x, float y, float w, float h, float r, float g, float b, float a);
/// @brief Internal: queue a screen-space textured quad sampling `pixels` over UV (0,0)-(1,1).
/// @param c Borrowed Canvas3D receiving the overlay command.
/// @param x Left destination edge in screen pixels.
/// @param y Top destination edge in screen pixels.
/// @param w Destination width in screen pixels.
/// @param h Destination height in screen pixels.
/// @param pixels Borrowed Pixels object retained through deferred replay.
/// @return Non-zero when the image command is queued; otherwise zero.
int canvas3d_queue_screen_image(rt_canvas3d *c, float x, float y, float w, float h, void *pixels);
/// @brief Internal: register the Canvas3D Game.UI widget draw-ops binding (ADR 0065).
void canvas3d_register_gameui_ops(void);
/* Plan 07: clustered forward+ binning — declarations live in
 * rt_canvas3d_clusters.h (they use backend types this header stays free of). */
/// @brief Internal: queue a screen-space rounded rectangle as a triangle fan (Plan 08).
/// @param c Borrowed Canvas3D receiving the overlay command.
/// @param x Left edge in screen pixels.
/// @param y Top edge in screen pixels.
/// @param w Width in screen pixels.
/// @param h Height in screen pixels.
/// @param radius Corner radius in pixels.
/// @param r Normalized red component.
/// @param g Normalized green component.
/// @param b Normalized blue component.
/// @param a Normalized alpha component.
/// @return Non-zero when visible fan geometry is queued; otherwise zero.
int canvas3d_queue_screen_round_rect(rt_canvas3d *c,
                                     float x,
                                     float y,
                                     float w,
                                     float h,
                                     float radius,
                                     float r,
                                     float g,
                                     float b,
                                     float a);
/// @brief Internal: queue a screen-space textured quad sampling `pixels` over an explicit
/// normalized UV sub-rect (u0,v0)-(u1,v1). Plan 08 region blits build on this.
/// @param c Borrowed Canvas3D receiving the overlay command.
/// @param x Left destination edge in screen pixels.
/// @param y Top destination edge in screen pixels.
/// @param w Destination width in screen pixels.
/// @param h Destination height in screen pixels.
/// @param pixels Borrowed Pixels object retained through deferred replay.
/// @param u0 Source U coordinate at the left edge.
/// @param v0 Source V coordinate at the top edge.
/// @param u1 Source U coordinate at the right edge.
/// @param v1 Source V coordinate at the bottom edge.
/// @return Non-zero when the image command is queued; otherwise zero.
int canvas3d_queue_screen_image_uv(rt_canvas3d *c,
                                   float x,
                                   float y,
                                   float w,
                                   float h,
                                   void *pixels,
                                   float u0,
                                   float v0,
                                   float u1,
                                   float v1);
/// @brief Internal: retain a GC object referenced by a final-overlay draw until the overlay
/// replays.
/// @param c Canvas3D that owns the final-overlay lifetime.
/// @param obj Borrowed GC-managed object to retain.
/// @return Non-zero when already tracked or retained successfully.
int canvas3d_track_final_overlay_temp_object(rt_canvas3d *c, void *obj);
/// @brief Internal: untrack and release a final-overlay temp object after a queueing failure.
/// @param c Canvas3D that owns the tracking set.
/// @param obj Tracked object whose overlay reference is released.
void canvas3d_release_tracked_final_overlay_temp_object(rt_canvas3d *c, void *obj);
/// @brief Internal: apply a height-weighted XZ wind sway to a mesh's vertices in place.
/// @details Base vertices (lowest local-Y) stay planted; displacement scales with
///   the square of normalized height along (dir_x, dir_z), modulated by sin(phase). Marks geometry
///   dirty. NULL/degenerate-safe. Exposed (non-static) so wind deformation is unit-testable.
/// @param mesh Mutable Mesh3D to deform in place.
/// @param dir_x Horizontal sway-direction X component.
/// @param dir_z Horizontal sway-direction Z component.
/// @param strength Maximum displacement scale; non-positive values are a no-op.
/// @param phase Animation phase in radians.
void canvas3d_deform_mesh_wind(
    rt_mesh3d *mesh, double dir_x, double dir_z, double strength, double phase);
/// @brief Internal: queue a 2D line into the overlay pass with thickness and RGBA color.
/// @param c Borrowed Canvas3D receiving the overlay command.
/// @param x0 First endpoint X coordinate in screen pixels.
/// @param y0 First endpoint Y coordinate in screen pixels.
/// @param x1 Second endpoint X coordinate in screen pixels.
/// @param y1 Second endpoint Y coordinate in screen pixels.
/// @param thickness Line thickness in pixels.
/// @param r Normalized red component.
/// @param g Normalized green component.
/// @param b Normalized blue component.
/// @param a Normalized alpha component.
/// @return Non-zero when line geometry is queued; otherwise zero.
int canvas3d_queue_screen_line(rt_canvas3d *c,
                               float x0,
                               float y0,
                               float x1,
                               float y1,
                               float thickness,
                               float r,
                               float g,
                               float b,
                               float a);
/// @brief Internal: discard recorded final-overlay commands and their temp buffers.
/// @param c Canvas3D whose retained overlay state is reset.
void canvas3d_clear_final_overlay(rt_canvas3d *c);

/// @brief Per-object previous-frame transforms for motion-vector derivation.
typedef struct {
    uintptr_t key;
    float current_model[16];
    float prev_model[16];
    int64_t last_frame_seen;
    int8_t has_current;
    int8_t has_prev;
} canvas_motion_history_t;

// Motion-history (rt_canvas3d_motion.c) + the shared open-addressing hash-table
// utilities, also used by the transient-resource tracker.
/// @brief Mix a pointer-sized identity into a well-distributed 32-bit hash.
/// @param value Integer or pointer identity to hash.
/// @return Deterministic 32-bit hash suitable for open-addressing tables.
uint32_t canvas3d_hash_u64(uintptr_t value);
/// @brief Round a positive table-size request up to a representable power of two.
/// @param value Requested minimum capacity.
/// @return Smallest supported power of two at least @p value, subject to the
///   implementation's saturation behavior.
int32_t canvas3d_next_power_of_two_i32(int32_t value);
/// @brief Drop all retained previous-model matrices after an external coordinate-space shift.
/// @details Floating-origin rebases change every world transform by the same delta. Retaining
///   history across that discontinuity would compare matrices from different origins, producing
///   bogus motion vectors and temporal shimmer.
/// @param c Canvas3D whose temporal transform table is cleared.
void canvas3d_clear_motion_history(rt_canvas3d *c);
/// @brief Drop all CPU occlusion covered-streak history after a visibility-space discontinuity.
/// @details CPU occlusion culling is deliberately history-gated. Floating-origin rebases, camera
///          cuts, projection/viewport changes, and render-target switches invalidate the prior
///          projected coverage relation, so retaining covered streaks across them can hide visible
///          triangles for one or more frames.
/// @param c Canvas3D whose occlusion history and duplicate counters are cleared.
void canvas3d_clear_occlusion_history(rt_canvas3d *c);
/// @brief Remove motion-history entries older than the canvas retention window.
/// @param c Canvas3D whose table is compacted for the current frame serial.
void canvas3d_prune_motion_history(rt_canvas3d *c);
/// @brief Resolve and update the previous model matrix for one stable motion key.
/// @details The current matrix becomes the retained history value for the next
///   frame. First sightings return the current transform with no previous flag.
/// @param c Canvas3D owning the temporal history table.
/// @param motion_key Non-zero stable identity for the logical draw instance.
/// @param current_model Borrowed current row-major 16-float model matrix.
/// @param out_prev_model Non-`NULL` array receiving the previous or fallback matrix.
/// @param out_has_prev Non-`NULL` flag set when genuine previous-frame data exists.
void canvas3d_resolve_previous_model(rt_canvas3d *c,
                                     uintptr_t motion_key,
                                     const float *current_model,
                                     float *out_prev_model,
                                     int8_t *out_has_prev);
/// @brief Derive a stable temporal key for a mesh/material/transform object tuple.
/// @param mesh_obj Borrowed Mesh3D identity.
/// @param material_obj Borrowed Material3D identity.
/// @param transform_obj Borrowed Mat4 identity.
/// @return Non-zero mixed key salted against runtime object reuse.
uintptr_t canvas3d_mesh_transform_motion_key(const void *mesh_obj,
                                             const void *material_obj,
                                             const void *transform_obj);
/// @brief Derive a stable temporal key for one instance in a submitted batch.
/// @param mesh_obj Borrowed shared Mesh3D identity.
/// @param material_obj Borrowed shared Material3D identity.
/// @param batch_obj Borrowed stable identity of the original matrix batch.
/// @param instance_count Original number of instances in the unsplit batch.
/// @param index Original zero-based instance index.
/// @return Non-zero mixed per-instance key salted against object reuse.
uintptr_t canvas3d_instance_motion_key(const void *mesh_obj,
                                       const void *material_obj,
                                       const void *batch_obj,
                                       int32_t instance_count,
                                       int32_t index);

// Shared finite-range check (rt_canvas3d.c) used by the geometry snapshotter.
/// @brief Test whether a double can be represented as a finite backend float.
/// @param value Double-precision value to test.
/// @return Non-zero when @p value is finite and within the float range.
int canvas3d_double_fits_float(double value);

/// @brief Record one Canvas3D submission failure in the sticky public diagnostics.
/// @details The latest status is replaced with @p status and the failure count increments with
///   saturation. A zero or unknown status is normalized to the generic resource failure class.
///   This helper does not trap, mutate queued commands, or release caller-owned resources.
/// @param c Canvas whose diagnostics receive the failure; NULL is ignored.
/// @param status One of the non-zero `RT_CANVAS3D_SUBMISSION_*` constants.
void canvas3d_record_submission_failure(rt_canvas3d *c, int32_t status);

/// @brief Arm a one-shot deferred-command reserve failure for deterministic CTest coverage.
/// @details The next main deferred-queue append consumes the flag and fails before publishing a
///   command, even when spare queue capacity already exists. Existing queued commands are not
///   changed. This is an implementation-only verification hook, not runtime registry surface.
/// @param canvas Canvas3D handle or approved stack fixture.
void rt_canvas3d_test_fail_next_queue_reserve(void *canvas);

/// @brief Arm a one-shot mesh-geometry snapshot failure for deterministic CTest coverage.
/// @details The next dynamic snapshot attempt consumes the flag, records snapshot diagnostics,
///   and returns before allocating or publishing snapshot storage. Existing commands and cached
///   snapshots remain valid. This is an implementation-only verification hook.
/// @param canvas Canvas3D handle or approved stack fixture.
void rt_canvas3d_test_fail_next_mesh_snapshot(void *canvas);

// Mesh-geometry snapshotting (rt_canvas3d_snapshot.c).
/// @brief Copy safe mesh geometry into frame-owned immutable snapshot storage.
/// @param c Canvas3D whose frame arena owns the copied arrays.
/// @param mesh Borrowed source mesh with validated geometry.
/// @param out_vertices Non-`NULL` output receiving the snapshot vertex pointer.
/// @param out_indices Non-`NULL` output receiving the snapshot index pointer.
/// @return Non-zero on success; zero on invalid geometry, limits, or allocation failure.
int canvas3d_snapshot_mesh_geometry(rt_canvas3d *c,
                                    const rt_mesh3d *mesh,
                                    vgfx3d_vertex_t **out_vertices,
                                    uint32_t **out_indices);
/// @brief Compute an axis-aligned bounding box over a vertex array.
/// @param vertices Borrowed vertex array.
/// @param vertex_count Number of readable vertices.
/// @param out_min Non-`NULL` three-component minimum output.
/// @param out_max Non-`NULL` three-component maximum output.
void canvas3d_compute_vertices_aabb(const vgfx3d_vertex_t *vertices,
                                    uint32_t vertex_count,
                                    float out_min[3],
                                    float out_max[3]);
/// @brief Snapshot mesh geometry while subtracting a double-precision origin from positions.
/// @param c Canvas3D whose frame storage owns the copied arrays.
/// @param mesh Borrowed source mesh, including optional authoritative double positions.
/// @param origin Borrowed three-double origin subtracted from every position.
/// @param out_vertices Non-`NULL` output receiving rebased snapshot vertices.
/// @param out_indices Non-`NULL` output receiving copied indices.
/// @return Non-zero on success; zero on invalid input, range failure, or allocation failure.
int canvas3d_snapshot_mesh_geometry_rebased(rt_canvas3d *c,
                                            const rt_mesh3d *mesh,
                                            const double origin[3],
                                            vgfx3d_vertex_t **out_vertices,
                                            uint32_t **out_indices);
/// @brief Ensure the per-frame mesh snapshot cache can hold at least @p needed entries.
/// @param c Canvas3D owning the cache arrays.
/// @param needed Non-negative minimum entry capacity.
/// @return Non-zero when capacity is available; zero on invalid input or allocation failure.
int canvas3d_reserve_mesh_snapshot_cache(rt_canvas3d *c, int32_t needed);
/// @brief Clear the snapshot-cache hash index without releasing retained entry payloads.
/// @param c Canvas3D owning the hash table.
void canvas3d_mesh_snapshot_hash_clear(rt_canvas3d *c);
/// @brief Reuse or build one frame snapshot for a mutable mesh object.
/// @param c Canvas3D owning the per-frame cache and snapshot storage.
/// @param mesh Borrowed validated Mesh3D payload.
/// @param mesh_obj Borrowed runtime identity used as the cache key.
/// @param out_vertices Non-`NULL` output receiving cached snapshot vertices.
/// @param out_indices Non-`NULL` output receiving cached snapshot indices.
/// @return Non-zero when a valid cached or new snapshot is returned.
int canvas3d_snapshot_mesh_geometry_cached(rt_canvas3d *c,
                                           rt_mesh3d *mesh,
                                           void *mesh_obj,
                                           vgfx3d_vertex_t **out_vertices,
                                           uint32_t **out_indices);
/// @brief Reuse or build a frame snapshot whose vertex stream includes generated tangents.
/// @param c Canvas3D owning the per-frame cache and snapshot storage.
/// @param mesh Borrowed validated Mesh3D payload.
/// @param mesh_obj Borrowed runtime identity used as the cache key.
/// @param out_vertices Non-`NULL` output receiving tangent-ready snapshot vertices.
/// @param out_indices Non-`NULL` output receiving cached snapshot indices.
/// @return Non-zero when tangent-ready geometry is returned.
int canvas3d_snapshot_mesh_geometry_with_tangents_cached(rt_canvas3d *c,
                                                         rt_mesh3d *mesh,
                                                         void *mesh_obj,
                                                         vgfx3d_vertex_t **out_vertices,
                                                         uint32_t **out_indices);
/// @brief Drop the canvas references held by retained mesh revisions in its frame snapshot table.
/// @param c Canvas whose current snapshot entries are about to be reset or destroyed.
void canvas3d_release_retained_mesh_revisions(rt_canvas3d *c);
/// @brief Decide whether a draw must snapshot geometry instead of borrowing live storage.
/// @param mesh Borrowed Mesh3D whose storage and mutation mode are inspected.
/// @param mesh_obj Borrowed runtime handle corresponding to @p mesh.
/// @return Non-zero when deferred submission cannot safely borrow the live arrays.
int canvas3d_should_snapshot_geometry(const rt_mesh3d *mesh, void *mesh_obj);

// Shared pixel utilities (rt_canvas3d.c) used by the CPU skybox.
/// @brief Convert a normalized float channel to an unsigned byte with clamping.
/// @param value Floating-point channel value.
/// @return Nearest unsigned byte after non-finite input becomes zero and the
///   channel is clamped to the inclusive normalized range.
uint8_t canvas3d_clamp01_to_u8(float value);
/// @brief Validate dimensions and row stride for an RGBA8 image layout.
/// @param w Image width in pixels.
/// @param h Image height in pixels.
/// @param stride Row stride in bytes.
/// @return Non-zero when dimensions are positive and the full layout is representable.
int canvas3d_rgba8_stride_valid(int32_t w, int32_t h, int32_t stride);

// CPU skybox fallback (rt_canvas3d_skybox.c).
/// @brief Ensure a canvas-owned CPU skybox cache matches the requested output size.
/// @param c Canvas3D owning the cached pixel buffer.
/// @param w Requested positive width in pixels.
/// @param h Requested positive height in pixels.
/// @return Non-zero when a current cache is available; zero on invalid input or failure.
int canvas3d_ensure_skybox_cpu_cache(rt_canvas3d *c, int32_t w, int32_t h);
/// @brief Copy the cached CPU skybox into an RGBA8 destination layout.
/// @param c Borrowed Canvas3D containing a compatible skybox cache.
/// @param dst_pixels Mutable destination byte buffer.
/// @param dst_w Destination width in pixels.
/// @param dst_h Destination height in pixels.
/// @param dst_stride Destination row stride in bytes.
void canvas3d_blit_skybox_cpu_cache(
    rt_canvas3d *c, uint8_t *dst_pixels, int32_t dst_w, int32_t dst_h, int32_t dst_stride);

// Value-sanitizing utilities (rt_canvas3d.c) used by the light flattener.
/// @brief Clamp a finite double to the normalized float interval.
/// @param value Value to convert.
/// @return Float in the inclusive range zero through one.
float canvas3d_clamp01_f64(double value);
/// @brief Report whether a canvas currently rebases uploads around the camera.
/// @param c Borrowed Canvas3D to inspect.
/// @return Non-zero when camera-relative upload is active.
int canvas3d_uses_camera_relative_upload(const rt_canvas3d *c);
/// @brief Convert a finite non-negative double to float or use a fallback.
/// @param value Preferred value.
/// @param fallback Float returned for non-finite or negative input.
/// @return Sanitized non-negative float.
float canvas3d_sanitize_nonnegative_f64(double value, float fallback);
/// @brief Convert a finite float-range double or use a fallback.
/// @param value Preferred value.
/// @param fallback Float returned when @p value is non-finite or out of range.
/// @return Converted value or @p fallback.
float canvas3d_sanitize_f64_to_float(double value, float fallback);
/// @brief Sanitize and clamp a double to caller-supplied bounds.
/// @param value Preferred value.
/// @param lo Inclusive lower bound.
/// @param hi Inclusive upper bound.
/// @param fallback Float returned for invalid input or bounds.
/// @return Finite float clamped to the requested interval, or @p fallback.
float canvas3d_clamp_f64_to_float(double value, double lo, double hi, float fallback);

// Light flattening (rt_canvas3d_lighting.c).
/// @brief Resolve the active per-draw light limit for the selected renderer path.
/// @param c Borrowed Canvas3D whose backend and clustered-lighting state are inspected.
/// @return Non-negative maximum number of lights considered for a draw.
int32_t canvas3d_active_light_limit(rt_canvas3d *c);

/// @brief Effective directional-shadow coverage distance for this canvas (rt_canvas3d.c).
/// @details Resolves the auto default (min(camera far, 300)) when shadow_distance is 0 and
///   clamps explicit values to the camera far plane. Always returns a positive finite
///   distance usable for cascade-split capping and shadow-caster sweep extrusion.
/// @param c Borrowed Canvas3D supplying shadow and cached camera distances.
/// @return Positive finite effective distance in world units.
float canvas3d_effective_shadow_distance(const rt_canvas3d *c);

/// @brief Refresh the shadow-caster sweep vector from the canvas + scene light slots
///   (rt_canvas3d.c). Call after scene lights are collected, before draw traversal.
/// @param c Mutable Canvas3D whose sweep vector and validity state are updated.
void canvas3d_update_shadow_caster_sweep(rt_canvas3d *c);

/// @brief Monotonic Light3D mutation generation (rt_light3d.c); never returns 0.
/// @details The per-frame flattened-light cache compares this against the generation it
///   flattened at, so any Light3D property change forces one re-flatten instead of
///   per-draw rebuilds.
/// @return Current non-zero process-wide light mutation generation.
uint64_t rt_light3d_mutation_revision(void);

/// @brief Drop the canvas's per-frame flattened-light cache.
/// @details Call after any change build_light_params reads that the Light3D mutation
///   generation cannot observe: canvas light-slot assignment, ambient color, scene-light
///   swaps, and camera-relative-origin changes at frame begin.
/// @param c Mutable Canvas3D whose per-frame flattened-light cache is invalidated.
static inline void canvas3d_invalidate_light_flatten_cache(rt_canvas3d *c) {
    if (c)
        c->frame_light_cache_valid = 0;
}

// Per-frame transient-resource tracking (rt_canvas3d_tempmgr.c): temp buffers,
// final-overlay temp buffers, and the GC-managed transient-object hash set.
/// @brief Transfer a heap buffer into the canvas's ordinary frame lifetime.
/// @param c Canvas3D that will free the buffer during frame cleanup.
/// @param buffer Owned allocation to track; ownership transfers only on success.
/// @return Non-zero when tracked; zero for invalid input or tracking allocation failure.
int canvas3d_track_temp_buffer(rt_canvas3d *c, void *buffer);
/// @brief Remove a frame buffer from tracking without freeing it.
/// @param c Canvas3D owning the tracking list.
/// @param buffer Borrowed buffer identity to remove.
/// @return Non-zero when a matching entry was removed.
int canvas3d_untrack_temp_buffer(rt_canvas3d *c, void *buffer);
/// @brief Remove and free one ordinary frame-tracked buffer.
/// @param c Canvas3D owning the tracking list.
/// @param buffer Tracked allocation to release; absent values are ignored.
void canvas3d_release_tracked_temp_buffer(rt_canvas3d *c, void *buffer);
/// @brief Release a tracked vertex/index snapshot and update snapshot-byte accounting.
/// @param c Canvas3D owning both allocations and diagnostic counters.
/// @param vertices Tracked vertex allocation, or `NULL`.
/// @param vertex_bytes Accounted size of @p vertices.
/// @param indices Tracked index allocation, or `NULL`.
/// @param index_bytes Accounted size of @p indices.
void canvas3d_release_tracked_mesh_snapshot(
    rt_canvas3d *c, void *vertices, size_t vertex_bytes, void *indices, size_t index_bytes);
/// @brief Transfer a heap buffer into the retained final-overlay lifetime.
/// @param c Canvas3D that will free the buffer after overlay replay/reset.
/// @param buffer Owned allocation to track; ownership transfers only on success.
/// @return Non-zero when tracked; zero for invalid input or allocation failure.
int canvas3d_track_final_overlay_temp_buffer(rt_canvas3d *c, void *buffer);
/// @brief Remove a final-overlay buffer from tracking without freeing it.
/// @param c Canvas3D owning the overlay tracking list.
/// @param buffer Borrowed buffer identity to remove.
/// @return Non-zero when a matching entry was removed.
int canvas3d_untrack_final_overlay_temp_buffer(rt_canvas3d *c, void *buffer);
/// @brief Remove and free one final-overlay-tracked buffer.
/// @param c Canvas3D owning the overlay tracking list.
/// @param buffer Tracked allocation to release; absent values are ignored.
void canvas3d_release_tracked_final_overlay_temp_buffer(rt_canvas3d *c, void *buffer);
/// @brief Allocate stable storage from the retained final-overlay vertex/index arena.
/// @details The arena is only used for HUD commands that replay after the normal frame temp
/// cleanup. Returned memory remains valid until @ref canvas3d_clear_final_overlay resets the arena,
/// and the helper deliberately avoids moving existing storage while a final overlay is being
/// recorded so previously queued draw commands keep valid pointers.
/// @param c Canvas that owns the final-overlay arena.
/// @param bytes Number of payload bytes to reserve.
/// @param alignment Power-of-two byte alignment for the returned pointer.
/// @return Pointer to arena storage, or NULL when the request cannot be satisfied without moving
/// existing queued geometry.
void *canvas3d_alloc_final_overlay_arena(rt_canvas3d *c, size_t bytes, size_t alignment);
/// @brief Reset retained final-overlay arena state after overlay replay.
/// @details This does not normally free the arena; it drops the used byte count so the next overlay
/// can reuse the same stable memory. Oversized arenas are released to avoid retaining large
/// transient captures after unusual frames.
/// @param c Canvas whose final-overlay arena should be reset.
void canvas3d_reset_final_overlay_arena(rt_canvas3d *c);
/// @brief Clear the transient-object membership hash without releasing objects.
/// @param c Canvas3D owning the hash table.
void canvas3d_temp_object_set_clear(rt_canvas3d *c);
/// @brief Ensure transient-object membership storage for an anticipated live count.
/// @param c Canvas3D owning the membership table.
/// @param count_hint Non-negative anticipated number of tracked objects.
/// @return Non-zero when suitable storage exists.
int canvas3d_ensure_temp_object_set(rt_canvas3d *c, int32_t count_hint);
/// @brief Test whether a GC object is already in the transient-object membership set.
/// @param c Borrowed Canvas3D whose set is queried.
/// @param obj Borrowed object identity to find.
/// @return Non-zero when the object is present.
int canvas3d_temp_object_set_contains(rt_canvas3d *c, void *obj);
/// @brief Insert a GC object identity into the transient membership set.
/// @param c Canvas3D owning the set.
/// @param obj Borrowed non-`NULL` object identity.
/// @return Non-zero when present after the call, including an existing entry.
int canvas3d_temp_object_set_insert(rt_canvas3d *c, void *obj);
/// @brief Rebuild transient-object membership from the authoritative retained list.
/// @param c Canvas3D whose membership index is reconstructed.
void canvas3d_rebuild_temp_object_set(rt_canvas3d *c);
/// @brief Retain a GC object through ordinary frame cleanup, deduplicating identities.
/// @param c Canvas3D that owns the temporary reference.
/// @param obj Borrowed GC-managed object to retain.
/// @return Non-zero when already tracked or newly retained successfully.
int canvas3d_track_temp_object(rt_canvas3d *c, void *obj);
/// @brief Untrack and release one ordinary frame-retained GC object.
/// @param c Canvas3D owning the temporary reference.
/// @param obj Tracked object to release; absent identities are ignored.
void canvas3d_release_tracked_temp_object(rt_canvas3d *c, void *obj);
/// @brief Free all ordinary frame-tracked native buffers.
/// @param c Canvas3D whose buffer list is emptied.
void canvas3d_clear_temp_buffers(rt_canvas3d *c);
/// @brief Release all ordinary frame-retained GC objects and reset membership.
/// @param c Canvas3D whose object list is emptied.
void canvas3d_clear_temp_objects(rt_canvas3d *c);
/// @brief Release every persistent DrawText2DAA raster owned by @p c.
/// @param c Canvas3D whose anti-aliased text cache is cleared.
void canvas3d_clear_aa_text_cache(rt_canvas3d *c);

/// @brief Resolve the vgfx frame limiter for Canvas3D's selected presentation path.
/// @details Native GPU backends pace through swap-interval/display-sync APIs and must leave
///          vgfx unlimited to avoid a second independent limiter. The software backend retains
///          the window's configured creation limit while requested vsync is enabled.
/// @param software_backend Non-zero when the selected renderer is the software backend.
/// @param vsync_enabled Non-zero when presentation synchronization was requested.
/// @param software_frame_limit Configured software-window frame limit in frames per second.
/// @return @p software_frame_limit only for synchronized software rendering;
///   otherwise negative one to disable the vgfx limiter.
static inline int32_t canvas3d_window_pacing_fps(int software_backend,
                                                 int vsync_enabled,
                                                 int32_t software_frame_limit) {
    return software_backend && vsync_enabled ? software_frame_limit : -1;
}
#ifdef __cplusplus
extern "C" {
#endif
/// @brief Bump-allocate @p bytes of frame-transient storage (16-byte aligned).
/// @details Chunked: growth adds chunks and never relocates, so recorded draw
///   commands keep valid pointers until the end-of-frame reset. Returns NULL
///   only on allocation failure (callers treat it like a malloc failure).
/// @param c Canvas3D owning the frame arena.
/// @param bytes Number of payload bytes to allocate; implementation-defined
///   zero-size handling applies.
/// @return Stable 16-byte-aligned frame storage, or `NULL` on failure.
void *canvas3d_frame_arena_alloc(rt_canvas3d *c, size_t bytes);
/// @brief Rewind the frame arena (frame flush); retains a bounded chunk set.
/// @param c Canvas3D whose arena allocations are invalidated for reuse.
void canvas3d_frame_arena_reset(rt_canvas3d *c);
/// @brief Free every frame-arena chunk (canvas teardown).
/// @param c Canvas3D whose arena storage is destroyed.
void canvas3d_frame_arena_free(rt_canvas3d *c);
#ifdef __cplusplus
}
#endif

#endif /* ZANNA_ENABLE_GRAPHICS */
