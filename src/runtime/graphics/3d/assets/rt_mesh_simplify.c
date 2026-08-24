//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_mesh_simplify.c
// Purpose: From-scratch quadric-error-metric (Garland-Heckbert) mesh
//   simplification powering Mesh3D.Simplify and SceneNode.GenerateLODs.
//   Collapses use subset placement (an edge collapses onto one endpoint), so
//   vertex attributes — UVs, normals, colors, bone weights — are never
//   interpolated and skinned meshes decimate safely.
//
// Key invariants:
//   - Vertices weld on the FULL record (position + attributes, exact bytes):
//     attribute seams therefore split topologically and inherit the open-border
//     quadric penalty, which is what keeps texture seams from swimming.
//   - Boundary/seam edges receive a x10 perpendicular-plane quadric penalty.
//   - Collapses that flip any surviving triangle's normal are rejected.
//   - Deterministic: costs tie-break on (min index, max index); no randomness.
//   - The result is always a NEW mesh; the input is never mutated.
//
// Ownership/Lifetime:
//   - Returned meshes are ordinary GC-managed Mesh3D objects whose finalizer
//     frees the vertex/index arrays this file allocates.
//
// Links: rt_mesh_simplify.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_mesh_simplify.c
 * @brief Implements deterministic, attribute-preserving QEM mesh simplification and LOD creation.
 * @details Uses exact-record welding, subset endpoint placement, boundary and material penalties,
 *          manifold link-condition checks, face-orientation validation, and deterministic heap
 *          ordering. The implementation preserves supported vertex side streams and produces a
 *          new mesh without mutating the source.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_mesh_simplify.h"
#include "../anim/rt_morphtarget3d.h"
#include "../anim/rt_skeleton3d_internal.h"
#include "../render/rt_canvas3d.h"
#include "../render/rt_canvas3d_internal.h"
#include "../scene/rt_scene3d.h"
#include "../scene/rt_scene3d_internal.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rt_alloc_size.h"
#include "rt_object.h"
#include "rt_trap.h"
/// @brief Decrement a runtime object's reference count and report whether finalization is due.
/// @param obj Runtime object handle.
/// @return Non-zero when @p obj reached a zero reference count.
extern int rt_obj_release_check0(void *obj);
/// @brief Finalize and free a runtime object whose reference count reached zero.
/// @param obj Runtime object handle ready for destruction.
extern void rt_obj_free(void *obj);

/*==========================================================================
 * Quadrics
 *=========================================================================*/

/// @brief Symmetric 4x4 quadric stored as its 10 unique coefficients.
typedef struct {
    /// Coefficients `Q00`, `Q01`, `Q02`, and `Q03`.
    double a2, ab, ac, ad;
    /// Coefficients `Q11`, `Q12`, and `Q13`.
    double b2, bc, bd;
    /// Coefficients `Q22` and `Q23`.
    double c2, cd;
    /// Coefficient `Q33`.
    double d2;
} simp_quadric_t;

/// @brief Accumulate another symmetric error quadric in place.
/// @param[in,out] q Destination quadric.
/// @param[in] o Quadric whose coefficients are added to @p q.
static void quadric_add(simp_quadric_t *q, const simp_quadric_t *o) {
    q->a2 += o->a2;
    q->ab += o->ab;
    q->ac += o->ac;
    q->ad += o->ad;
    q->b2 += o->b2;
    q->bc += o->bc;
    q->bd += o->bd;
    q->c2 += o->c2;
    q->cd += o->cd;
    q->d2 += o->d2;
}

/// @brief Build a plane quadric (a,b,c,d normalized plane) scaled by @p weight.
/// @param[out] q Destination symmetric quadric.
/// @param a Plane normal X coefficient.
/// @param b Plane normal Y coefficient.
/// @param c Plane normal Z coefficient.
/// @param d Plane offset coefficient.
/// @param weight Scalar error weight applied to every outer-product coefficient.
static void quadric_from_plane(
    simp_quadric_t *q, double a, double b, double c, double d, double weight) {
    q->a2 = a * a * weight;
    q->ab = a * b * weight;
    q->ac = a * c * weight;
    q->ad = a * d * weight;
    q->b2 = b * b * weight;
    q->bc = b * c * weight;
    q->bd = b * d * weight;
    q->c2 = c * c * weight;
    q->cd = c * d * weight;
    q->d2 = d * d * weight;
}

/// @brief Evaluate the homogeneous quadratic form at one double-precision position.
/// @param[in] q Symmetric error quadric.
/// @param[in] p Three-component Cartesian position.
/// @return Homogeneous quadric error at `(p.x, p.y, p.z, 1)`.
static double quadric_eval(const simp_quadric_t *q, const double p[3]) {
    double x = p[0];
    double y = p[1];
    double z = p[2];
    return q->a2 * x * x + 2.0 * q->ab * x * y + 2.0 * q->ac * x * z + 2.0 * q->ad * x +
           q->b2 * y * y + 2.0 * q->bc * y * z + 2.0 * q->bd * y + q->c2 * z * z + 2.0 * q->cd * z +
           q->d2;
}

/*==========================================================================
 * Working set
 *=========================================================================*/

/// @brief One directed collapse candidate stored in the deterministic minimum heap.
typedef struct {
    /// Quadric cost of collapsing @ref b onto @ref a.
    double cost;
    /// Surviving endpoint candidate.
    uint32_t a; /* surviving endpoint candidate */
    /// Endpoint removed by the candidate collapse.
    uint32_t b; /* removed endpoint */
    /// Version of @ref a when this candidate was queued.
    uint32_t stamp_a;
    /// Version of @ref b when this candidate was queued.
    uint32_t stamp_b;
} simp_heap_entry_t;

/// @brief Mutable working storage for one simplification operation.
typedef struct {
    /// Borrowed source mesh supplying authoritative side streams.
    const rt_mesh3d *source_mesh;
    /// Welded fixed-record vertices.
    vgfx3d_vertex_t *verts;    /* welded working vertices */
    /// Representative source vertex for each welded endpoint.
    uint32_t *source_vertices; /* surviving source vertex for every welded endpoint */
    /// Accumulated error quadric for each working vertex.
    simp_quadric_t *quadrics;
    /// Vertex versions used to invalidate stale heap entries.
    uint32_t *stamps; /* bumped on every collapse touching the vertex */
    /// Per-working-vertex liveness flags.
    int8_t *alive;
    /// Number of welded working vertices.
    uint32_t vert_count;

    /// Flat working triangle array; retired faces use `UINT32_MAX`.
    uint32_t *tris;              /* 3 welded indices per face; alive when tris[i*3] != UINT32_MAX */
    /// Material classification for each source face.
    int32_t *tri_material_slots; /* source material slot per face, -1 when unclassified */
    /// Total number of source faces represented by @ref tris.
    uint32_t tri_count;
    /// Current number of non-retired faces.
    uint32_t live_tris;
    /// Whether validated source submesh ranges must be preserved.
    int8_t has_submesh_ranges;

    /* CSR adjacency: faces incident to each vertex (rebuilt lazily per collapse
     * via linked lists to keep collapses O(valence)). */
    /// Head corner-link index for each vertex.
    int32_t *vert_face_head; /* head index into face_links, -1 = none */
    /// Next-link table for every face corner.
    int32_t *face_links;     /* per (face,corner): next link */

    /// Generation-stamped vertex one-ring scratch table.
    uint32_t *vertex_marks; /* generation-stamped one-ring scratch */
    /// Next available vertex-mark generation.
    uint32_t vertex_mark_generation;
    /// Generation-stamped face traversal scratch table.
    uint32_t *face_marks; /* generation-stamped fan traversal scratch */
    /// Next available face-mark generation.
    uint32_t face_mark_generation;
    /// Breadth-first face queue with @ref tri_count entries.
    uint32_t *face_queue; /* tri_count-entry fan traversal queue */

    /// Owned deterministic minimum-heap entries.
    simp_heap_entry_t *heap;
    /// Number of live entries in @ref heap.
    uint32_t heap_count;
    /// Allocated entry capacity of @ref heap.
    uint32_t heap_capacity;

    /// Non-zero when classified boundary vertices may never be removed.
    int8_t lock_boundaries;
    /// Absolute quadric-cost ceiling; collapsing stops above it. Zero disables.
    double max_error_cap;
} simp_ctx_t;

/// @brief Exchange two entries in a simplification heap.
/// @param[in,out] h Heap entry array.
/// @param i First entry index.
/// @param j Second entry index.
static void simp_heap_swap(simp_heap_entry_t *h, uint32_t i, uint32_t j) {
    simp_heap_entry_t t = h[i];
    h[i] = h[j];
    h[j] = t;
}

/// @brief Deterministic ordering: cost, then (min,max) index pair.
/// @param[in] x First collapse candidate.
/// @param[in] y Second collapse candidate.
/// @return Non-zero when @p x sorts before @p y.
static int simp_heap_less(const simp_heap_entry_t *x, const simp_heap_entry_t *y) {
    if (x->cost != y->cost)
        return x->cost < y->cost;
    if (x->a != y->a)
        return x->a < y->a;
    return x->b < y->b;
}

/// @brief Insert a directed collapse candidate into the minimum heap.
/// @param[in,out] cx Simplification context owning the heap.
/// @param e Candidate entry copied into heap storage.
/// @return Non-zero on success; zero when heap growth allocation fails.
static int simp_heap_push(simp_ctx_t *cx, simp_heap_entry_t e) {
    if (cx->heap_count >= cx->heap_capacity) {
        uint32_t cap = cx->heap_capacity ? cx->heap_capacity * 2u : 256u;
        simp_heap_entry_t *grown =
            (simp_heap_entry_t *)realloc(cx->heap, (size_t)cap * sizeof(*grown));
        if (!grown)
            return 0;
        cx->heap = grown;
        cx->heap_capacity = cap;
    }
    uint32_t i = cx->heap_count++;
    cx->heap[i] = e;
    while (i > 0) {
        uint32_t parent = (i - 1) / 2;
        if (!simp_heap_less(&cx->heap[i], &cx->heap[parent]))
            break;
        simp_heap_swap(cx->heap, i, parent);
        i = parent;
    }
    return 1;
}

/// @brief Remove the least-cost deterministic collapse candidate from the heap.
/// @param[in,out] cx Simplification context owning the heap.
/// @param[out] out Receives the removed minimum entry.
/// @return Non-zero when an entry was removed; zero when the heap is empty.
static int simp_heap_pop(simp_ctx_t *cx, simp_heap_entry_t *out) {
    if (cx->heap_count == 0)
        return 0;
    *out = cx->heap[0];
    cx->heap[0] = cx->heap[--cx->heap_count];
    uint32_t i = 0;
    for (;;) {
        uint32_t l = i * 2 + 1;
        uint32_t r = l + 1;
        uint32_t smallest = i;
        if (l < cx->heap_count && simp_heap_less(&cx->heap[l], &cx->heap[smallest]))
            smallest = l;
        if (r < cx->heap_count && simp_heap_less(&cx->heap[r], &cx->heap[smallest]))
            smallest = r;
        if (smallest == i)
            break;
        simp_heap_swap(cx->heap, i, smallest);
        i = smallest;
    }
    return 1;
}

/**
 * @brief Extend a deterministic FNV-1a hash with an arbitrary byte range.
 * @param hash Existing hash state.
 * @param bytes Borrowed byte range; may be `NULL` only when @p size is zero.
 * @param size Number of bytes to consume.
 * @return Updated 32-bit hash state.
 */
static uint32_t simp_hash_bytes(uint32_t hash, const void *bytes, size_t size) {
    const uint8_t *input = (const uint8_t *)bytes;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ input[i]) * 16777619u;
    return hash;
}

/**
 * @brief Test whether two source vertices have identical retained side-stream payloads.
 *
 * Interleaved normal/tangent/UV/color/joint data is compared with exact bytes. Optional
 * authoritative double positions and influences 5–8 participate as well, preventing a weld
 * from discarding data that cannot be reconstructed from the fixed vertex record.
 *
 * @param mesh Valid source mesh.
 * @param a First source vertex index.
 * @param b Second source vertex index.
 * @return Non-zero only when every simplification-retained stream is byte-identical.
 */
static int simp_source_vertices_equal(const rt_mesh3d *mesh, uint32_t a, uint32_t b) {
    if (!mesh || memcmp(&mesh->vertices[a], &mesh->vertices[b], sizeof(vgfx3d_vertex_t)) != 0)
        return 0;
    if (mesh->positions64 && memcmp(&mesh->positions64[(size_t)a * 3u],
                                    &mesh->positions64[(size_t)b * 3u],
                                    3u * sizeof(double)) != 0)
        return 0;
    if (mesh->extra_influences && memcmp(&mesh->extra_influences[a],
                                         &mesh->extra_influences[b],
                                         sizeof(vgfx3d_extra_influences_t)) != 0)
        return 0;
    return 1;
}

/**
 * @brief Exact-record weld that preserves every retained per-vertex side stream.
 *
 * Attribute seams remain split because any differing fixed-record, double-position, or extra
 * influence byte prevents a weld. Attached morph targets conservatively disable cross-index
 * welding because their optional tangent channels are private to the owning module; shared source
 * indices still retain ordinary indexed topology. `out_sources[w]` records the representative
 * source vertex used later to remap side streams after subset collapses.
 *
 * @param mesh Valid source mesh with @p count readable vertices.
 * @param count Safe source vertex count.
 * @param out_welded Receives an exact-size-or-larger welded fixed-record array.
 * @param out_sources Receives one representative source index per welded vertex.
 * @param out_welded_count Receives the number of populated welded records.
 * @return Source-to-welded map, or `NULL` on allocation failure.
 */
static uint32_t *simp_weld(const rt_mesh3d *mesh,
                           uint32_t count,
                           vgfx3d_vertex_t **out_welded,
                           uint32_t **out_sources,
                           uint32_t *out_welded_count) {
    uint32_t *remap = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    vgfx3d_vertex_t *welded = (vgfx3d_vertex_t *)malloc((size_t)count * sizeof(*welded));
    uint32_t *sources = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    uint32_t table_size = 1;
    while (table_size < count * 2u + 1u)
        table_size <<= 1;
    uint32_t *table = (uint32_t *)malloc((size_t)table_size * sizeof(uint32_t));
    if (!mesh || !out_welded || !out_sources || !out_welded_count || !remap || !welded ||
        !sources || !table) {
        free(remap);
        free(welded);
        free(sources);
        free(table);
        return NULL;
    }
    memset(table, 0xFF, (size_t)table_size * sizeof(uint32_t));
    uint32_t welded_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t hash = 2166136261u;
        hash = simp_hash_bytes(hash, &mesh->vertices[i], sizeof(vgfx3d_vertex_t));
        if (mesh->positions64) {
            hash = simp_hash_bytes(hash, &mesh->positions64[(size_t)i * 3u], 3u * sizeof(double));
        }
        if (mesh->extra_influences) {
            hash = simp_hash_bytes(
                hash, &mesh->extra_influences[i], sizeof(vgfx3d_extra_influences_t));
        }
        uint32_t slot = hash & (table_size - 1u);
        uint32_t found = UINT32_MAX;
        while (!mesh->morph_targets_ref && table[slot] != UINT32_MAX) {
            uint32_t cand = table[slot];
            if (simp_source_vertices_equal(mesh, sources[cand], i)) {
                found = cand;
                break;
            }
            slot = (slot + 1u) & (table_size - 1u);
        }
        if (found == UINT32_MAX) {
            while (table[slot] != UINT32_MAX)
                slot = (slot + 1u) & (table_size - 1u);
            welded[welded_count] = mesh->vertices[i];
            sources[welded_count] = i;
            table[slot] = welded_count;
            found = welded_count++;
        }
        remap[i] = found;
    }
    free(table);
    *out_welded = welded;
    *out_sources = sources;
    *out_welded_count = welded_count;
    return remap;
}

/**
 * @brief Read one working endpoint's authoritative source position as doubles.
 * @param cx Active simplification context.
 * @param vertex Working/welded vertex index.
 * @param out_position Receives XYZ; cleared for invalid input.
 */
static void simp_vertex_position(const simp_ctx_t *cx, uint32_t vertex, double out_position[3]) {
    uint32_t source = 0;
    if (!out_position)
        return;
    out_position[0] = out_position[1] = out_position[2] = 0.0;
    if (!cx || !cx->source_mesh || !cx->source_vertices || vertex >= cx->vert_count)
        return;
    source = cx->source_vertices[vertex];
    if (cx->source_mesh->positions64) {
        out_position[0] = cx->source_mesh->positions64[(size_t)source * 3u + 0u];
        out_position[1] = cx->source_mesh->positions64[(size_t)source * 3u + 1u];
        out_position[2] = cx->source_mesh->positions64[(size_t)source * 3u + 2u];
    } else {
        out_position[0] = (double)cx->verts[vertex].pos[0];
        out_position[1] = (double)cx->verts[vertex].pos[1];
        out_position[2] = (double)cx->verts[vertex].pos[2];
    }
}

/// @brief Recompute a face's normal from authoritative positions; returns 0 for degenerate faces.
/// @param[in] cx Active simplification context.
/// @param f Source face index.
/// @param[out] n Receives the normalized face normal on success.
/// @return Non-zero for a finite, nondegenerate face; zero otherwise.
static int simp_face_normal(const simp_ctx_t *cx, uint32_t f, double n[3]) {
    double p0[3];
    double p1[3];
    double p2[3];
    simp_vertex_position(cx, cx->tris[f * 3 + 0], p0);
    simp_vertex_position(cx, cx->tris[f * 3 + 1], p1);
    simp_vertex_position(cx, cx->tris[f * 3 + 2], p2);
    double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    double len = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!isfinite(len) || len < 1e-14)
        return 0;
    n[0] /= len;
    n[1] /= len;
    n[2] /= len;
    return 1;
}

/// @brief Insert each corner of one live face into the vertex-incidence lists.
/// @param[in,out] cx Simplification context owning the adjacency tables.
/// @param f Source face index whose three working vertices are linked.
static void simp_link_face(simp_ctx_t *cx, uint32_t f) {
    for (int c = 0; c < 3; c++) {
        uint32_t v = cx->tris[f * 3 + c];
        cx->face_links[f * 3 + c] = cx->vert_face_head[v];
        cx->vert_face_head[v] = (int32_t)(f * 3 + c);
    }
}

/// @brief Cost of collapsing b onto a (subset placement at a's position).
/// @param[in] cx Active simplification context.
/// @param a Surviving endpoint candidate.
/// @param b Removed endpoint candidate.
/// @return Combined endpoint quadric evaluated at @p a, clamped to zero when negative.
static double simp_collapse_cost(const simp_ctx_t *cx, uint32_t a, uint32_t b) {
    simp_quadric_t q = cx->quadrics[a];
    double position[3];
    quadric_add(&q, &cx->quadrics[b]);
    simp_vertex_position(cx, a, position);
    double c = quadric_eval(&q, position);
    return c < 0.0 ? 0.0 : c;
}

/// @brief Queue one directed edge-collapse candidate with current endpoint stamps.
/// @param[in,out] cx Simplification context owning the candidate heap.
/// @param a Surviving endpoint candidate.
/// @param b Removed endpoint candidate.
/// @note A self-edge is ignored, and heap allocation failure is intentionally deferred
///       as candidate exhaustion rather than reported separately.
static void simp_push_edge(simp_ctx_t *cx, uint32_t a, uint32_t b) {
    simp_heap_entry_t e;
    if (a == b)
        return;
    e.cost = simp_collapse_cost(cx, a, b);
    e.a = a;
    e.b = b;
    e.stamp_a = cx->stamps[a];
    e.stamp_b = cx->stamps[b];
    (void)simp_heap_push(cx, e);
}

/** @brief Local incidence summary for one undirected working edge. */
typedef struct {
    /// Number of live faces incident to the edge; may exceed two for rejection.
    uint32_t face_count;
    /// Opposite vertex from each of the first two incident faces.
    uint32_t opposite[2];
    /// Material slot from each of the first two incident faces.
    int32_t material_slots[2];
} simp_edge_info_t;

/**
 * @brief Return whether one source face remains live in the working triangle list.
 * @param cx Active simplification context.
 * @param face Source face index.
 * @return Non-zero when all storage is valid and the face has not been retired.
 */
static int simp_face_is_live(const simp_ctx_t *cx, uint32_t face) {
    return cx && cx->tris && face < cx->tri_count && cx->tris[(size_t)face * 3u] != UINT32_MAX;
}

/**
 * @brief Return whether a live face currently references one working vertex.
 * @param cx Active simplification context.
 * @param face Source face index.
 * @param vertex Working vertex index.
 * @return Non-zero when @p vertex occupies any of the face's three corners.
 */
static int simp_face_contains(const simp_ctx_t *cx, uint32_t face, uint32_t vertex) {
    const uint32_t *tri;
    if (!simp_face_is_live(cx, face))
        return 0;
    tri = &cx->tris[(size_t)face * 3u];
    return tri[0] == vertex || tri[1] == vertex || tri[2] == vertex;
}

/**
 * @brief Collect incident faces, opposite vertices, and material classes for an edge.
 *
 * The face count is allowed to exceed two so callers can reject non-manifold edges. Only the
 * first two opposite/material entries are retained because legal triangular edges have at most
 * two incident faces.
 *
 * @param cx Active simplification context.
 * @param a First endpoint.
 * @param b Second endpoint.
 * @param out_info Receives a zero-initialized incidence summary.
 */
static void simp_get_edge_info(const simp_ctx_t *cx,
                               uint32_t a,
                               uint32_t b,
                               simp_edge_info_t *out_info) {
    if (!out_info)
        return;
    memset(out_info, 0, sizeof(*out_info));
    out_info->material_slots[0] = -1;
    out_info->material_slots[1] = -1;
    if (!cx || !cx->vert_face_head || !cx->face_links || a >= cx->vert_count || b >= cx->vert_count)
        return;
    for (int32_t link = cx->vert_face_head[a]; link >= 0; link = cx->face_links[link]) {
        uint32_t face = (uint32_t)link / 3u;
        const uint32_t *tri;
        uint32_t opposite = UINT32_MAX;
        if (!simp_face_contains(cx, face, b))
            continue;
        tri = &cx->tris[(size_t)face * 3u];
        for (int corner = 0; corner < 3; ++corner) {
            if (tri[corner] != a && tri[corner] != b) {
                opposite = tri[corner];
                break;
            }
        }
        if (out_info->face_count < 2u) {
            out_info->opposite[out_info->face_count] = opposite;
            out_info->material_slots[out_info->face_count] =
                cx->tri_material_slots ? cx->tri_material_slots[face] : -1;
        }
        if (out_info->face_count != UINT32_MAX)
            out_info->face_count++;
    }
}

/**
 * @brief Classify an edge as an open/attribute/material boundary.
 * @param info Current edge incidence summary.
 * @return Non-zero for a one-face edge or a legal two-face edge whose material slots differ.
 */
static int simp_edge_is_classified_boundary(const simp_edge_info_t *info) {
    if (!info)
        return 0;
    if (info->face_count == 1u)
        return 1;
    return info->face_count == 2u && info->material_slots[0] != info->material_slots[1];
}

/**
 * @brief Reserve two adjacent non-zero generations in the vertex scratch table.
 * @param cx Mutable simplification context.
 * @return First generation value; the second is exactly one greater.
 */
static uint32_t simp_claim_vertex_mark_pair(simp_ctx_t *cx) {
    if (!cx || !cx->vertex_marks)
        return 0;
    if (cx->vertex_mark_generation == 0 || cx->vertex_mark_generation >= UINT32_MAX - 1u) {
        memset(cx->vertex_marks, 0, (size_t)cx->vert_count * sizeof(uint32_t));
        cx->vertex_mark_generation = 1u;
    }
    {
        uint32_t generation = cx->vertex_mark_generation;
        cx->vertex_mark_generation += 2u;
        return generation;
    }
}

/**
 * @brief Reserve one non-zero generation in the face traversal scratch table.
 * @param cx Mutable simplification context.
 * @return Generation value used for the next connected-fan walk.
 */
static uint32_t simp_claim_face_mark(simp_ctx_t *cx) {
    if (!cx || !cx->face_marks)
        return 0;
    if (cx->face_mark_generation == 0 || cx->face_mark_generation == UINT32_MAX) {
        memset(cx->face_marks, 0, (size_t)cx->tri_count * sizeof(uint32_t));
        cx->face_mark_generation = 1u;
    }
    return cx->face_mark_generation++;
}

/**
 * @brief Determine whether a working vertex lies on any classified boundary edge.
 * @param cx Active simplification context.
 * @param vertex Working vertex index.
 * @return Non-zero for an open, attribute-seam, or material-seam endpoint.
 */
static int simp_vertex_is_classified_boundary(const simp_ctx_t *cx, uint32_t vertex) {
    if (!cx || vertex >= cx->vert_count)
        return 0;
    for (int32_t link = cx->vert_face_head[vertex]; link >= 0; link = cx->face_links[link]) {
        uint32_t face = (uint32_t)link / 3u;
        const uint32_t *tri;
        if (!simp_face_is_live(cx, face))
            continue;
        tri = &cx->tris[(size_t)face * 3u];
        for (int corner = 0; corner < 3; ++corner) {
            simp_edge_info_t edge;
            uint32_t neighbor = tri[corner];
            if (neighbor == vertex)
                continue;
            simp_get_edge_info(cx, vertex, neighbor, &edge);
            if (simp_edge_is_classified_boundary(&edge))
                return 1;
        }
    }
    return 0;
}

/**
 * @brief Validate that one vertex's incident triangles form a connected manifold fan.
 *
 * Every spoke edge must have one or two incident triangles. A topological boundary fan has
 * exactly two one-face spokes; a closed fan has none. A generation-stamped breadth-first walk
 * then proves all incident faces belong to one component, rejecting bow-tie vertices even when
 * their aggregate edge counts happen to look manifold.
 *
 * @param cx Mutable context providing reusable scratch marks/queue.
 * @param vertex Working endpoint to validate.
 * @return Non-zero only for one connected open or closed triangular fan.
 */
static int simp_vertex_fan_is_manifold(simp_ctx_t *cx, uint32_t vertex) {
    uint32_t incident_count = 0;
    uint32_t boundary_spokes = 0;
    uint32_t first_face = UINT32_MAX;
    uint32_t vertex_generation;
    uint32_t face_generation;
    uint32_t queue_head = 0;
    uint32_t queue_tail = 0;
    uint32_t visited_count = 0;

    if (!cx || vertex >= cx->vert_count || !cx->face_queue)
        return 0;
    vertex_generation = simp_claim_vertex_mark_pair(cx);
    face_generation = simp_claim_face_mark(cx);
    if (vertex_generation == 0 || face_generation == 0)
        return 0;

    for (int32_t link = cx->vert_face_head[vertex]; link >= 0; link = cx->face_links[link]) {
        uint32_t face = (uint32_t)link / 3u;
        const uint32_t *tri;
        if (!simp_face_is_live(cx, face))
            continue;
        if (first_face == UINT32_MAX)
            first_face = face;
        incident_count++;
        tri = &cx->tris[(size_t)face * 3u];
        for (int corner = 0; corner < 3; ++corner) {
            simp_edge_info_t edge;
            uint32_t neighbor = tri[corner];
            if (neighbor == vertex || cx->vertex_marks[neighbor] == vertex_generation)
                continue;
            cx->vertex_marks[neighbor] = vertex_generation;
            simp_get_edge_info(cx, vertex, neighbor, &edge);
            if (edge.face_count == 0u || edge.face_count > 2u)
                return 0;
            if (edge.face_count == 1u)
                boundary_spokes++;
        }
    }
    if (incident_count == 0u || (boundary_spokes != 0u && boundary_spokes != 2u))
        return 0;

    cx->face_marks[first_face] = face_generation;
    cx->face_queue[queue_tail++] = first_face;
    while (queue_head < queue_tail) {
        uint32_t face = cx->face_queue[queue_head++];
        const uint32_t *tri = &cx->tris[(size_t)face * 3u];
        visited_count++;
        for (int corner = 0; corner < 3; ++corner) {
            uint32_t neighbor = tri[corner];
            if (neighbor == vertex)
                continue;
            for (int32_t link = cx->vert_face_head[vertex]; link >= 0;
                 link = cx->face_links[link]) {
                uint32_t adjacent_face = (uint32_t)link / 3u;
                if (!simp_face_is_live(cx, adjacent_face) ||
                    cx->face_marks[adjacent_face] == face_generation ||
                    !simp_face_contains(cx, adjacent_face, neighbor))
                    continue;
                cx->face_marks[adjacent_face] = face_generation;
                if (queue_tail >= cx->tri_count)
                    return 0;
                cx->face_queue[queue_tail++] = adjacent_face;
            }
        }
    }
    return visited_count == incident_count;
}

/**
 * @brief Enforce the manifold link condition and classified-boundary preservation.
 *
 * For a legal triangle edge, the endpoints' common one-ring is exactly the set of opposite
 * vertices in its one or two incident faces. Endpoint fans must already be connected manifolds.
 * Boundary edges collapse only boundary-to-boundary, while interior edges collapse only between
 * interior vertices.
 *
 * @param cx Mutable simplification context.
 * @param a Surviving endpoint candidate.
 * @param b Removed endpoint candidate.
 * @return Non-zero when the topological collapse is legal.
 */
static int simp_link_condition_holds(simp_ctx_t *cx, uint32_t a, uint32_t b) {
    simp_edge_info_t edge;
    uint32_t generation;
    uint32_t common_count = 0;
    int edge_boundary;
    int a_boundary;
    int b_boundary;

    simp_get_edge_info(cx, a, b, &edge);
    if (edge.face_count < 1u || edge.face_count > 2u || !simp_vertex_fan_is_manifold(cx, a) ||
        !simp_vertex_fan_is_manifold(cx, b))
        return 0;
    edge_boundary = simp_edge_is_classified_boundary(&edge);
    a_boundary = simp_vertex_is_classified_boundary(cx, a);
    b_boundary = simp_vertex_is_classified_boundary(cx, b);
    if (edge_boundary ? (!a_boundary || !b_boundary) : (a_boundary || b_boundary))
        return 0;

    generation = simp_claim_vertex_mark_pair(cx);
    if (generation == 0)
        return 0;
    for (int32_t link = cx->vert_face_head[b]; link >= 0; link = cx->face_links[link]) {
        uint32_t face = (uint32_t)link / 3u;
        const uint32_t *tri;
        if (!simp_face_is_live(cx, face))
            continue;
        tri = &cx->tris[(size_t)face * 3u];
        for (int corner = 0; corner < 3; ++corner) {
            uint32_t neighbor = tri[corner];
            if (neighbor != a && neighbor != b)
                cx->vertex_marks[neighbor] = generation;
        }
    }
    for (int32_t link = cx->vert_face_head[a]; link >= 0; link = cx->face_links[link]) {
        uint32_t face = (uint32_t)link / 3u;
        const uint32_t *tri;
        if (!simp_face_is_live(cx, face))
            continue;
        tri = &cx->tris[(size_t)face * 3u];
        for (int corner = 0; corner < 3; ++corner) {
            uint32_t neighbor = tri[corner];
            int expected = 0;
            if (neighbor == a || neighbor == b || cx->vertex_marks[neighbor] != generation)
                continue;
            cx->vertex_marks[neighbor] = generation + 1u;
            for (uint32_t i = 0; i < edge.face_count && i < 2u; ++i) {
                if (edge.opposite[i] == neighbor)
                    expected = 1;
            }
            if (!expected)
                return 0;
            common_count++;
        }
    }
    return common_count == edge.face_count;
}

/**
 * @brief Simulate replacing endpoint @p b with @p a in one live face.
 * @param cx Active simplification context.
 * @param face Source face index.
 * @param a Surviving endpoint.
 * @param b Removed endpoint.
 * @param out_triangle Receives the simulated three indices.
 * @return Non-zero for a surviving three-distinct-index face; zero for retired/vanishing faces.
 */
static int simp_simulate_face(
    const simp_ctx_t *cx, uint32_t face, uint32_t a, uint32_t b, uint32_t out_triangle[3]) {
    if (!out_triangle || !simp_face_is_live(cx, face))
        return 0;
    for (int corner = 0; corner < 3; ++corner) {
        uint32_t vertex = cx->tris[(size_t)face * 3u + (size_t)corner];
        out_triangle[corner] = vertex == b ? a : vertex;
    }
    return out_triangle[0] != out_triangle[1] && out_triangle[1] != out_triangle[2] &&
           out_triangle[0] != out_triangle[2];
}

/**
 * @brief Sort a triangle's indices into canonical order for duplicate comparison.
 * @param triangle Three vertex indices, reordered in place.
 */
static void simp_sort_triangle(uint32_t triangle[3]) {
    uint32_t tmp;
    if (triangle[0] > triangle[1]) {
        tmp = triangle[0];
        triangle[0] = triangle[1];
        triangle[1] = tmp;
    }
    if (triangle[1] > triangle[2]) {
        tmp = triangle[1];
        triangle[1] = triangle[2];
        triangle[2] = tmp;
    }
    if (triangle[0] > triangle[1]) {
        tmp = triangle[0];
        triangle[0] = triangle[1];
        triangle[1] = tmp;
    }
}

/**
 * @brief Reject a simulated face that degenerates or reverses orientation.
 *
 * Cross-product magnitude is compared to a scale-relative edge threshold, then the normalized
 * before/after normals must retain a positive dot product. This catches near-zero slivers as well
 * as exact degeneracy and inverted faces.
 *
 * @param cx Active simplification context.
 * @param face Original live face index.
 * @param triangle Simulated surviving indices.
 * @return Non-zero when area and orientation remain valid.
 */
static int simp_simulated_face_geometry_valid(const simp_ctx_t *cx,
                                              uint32_t face,
                                              const uint32_t triangle[3]) {
    double before[3];
    double p0[3];
    double p1[3];
    double p2[3];
    double e1[3];
    double e2[3];
    double e3[3];
    double after[3];
    double after_len;
    double max_edge_sq;
    double dot;
    if (!triangle || !simp_face_normal(cx, face, before))
        return 0;
    simp_vertex_position(cx, triangle[0], p0);
    simp_vertex_position(cx, triangle[1], p1);
    simp_vertex_position(cx, triangle[2], p2);
    for (int axis = 0; axis < 3; ++axis) {
        e1[axis] = p1[axis] - p0[axis];
        e2[axis] = p2[axis] - p0[axis];
        e3[axis] = p2[axis] - p1[axis];
    }
    after[0] = e1[1] * e2[2] - e1[2] * e2[1];
    after[1] = e1[2] * e2[0] - e1[0] * e2[2];
    after[2] = e1[0] * e2[1] - e1[1] * e2[0];
    after_len = hypot(hypot(after[0], after[1]), after[2]);
    max_edge_sq = fmax(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2],
                       fmax(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2],
                            e3[0] * e3[0] + e3[1] * e3[1] + e3[2] * e3[2]));
    if (!isfinite(after_len) || !isfinite(max_edge_sq) || max_edge_sq <= 0.0 ||
        after_len <= max_edge_sq * 1e-8)
        return 0;
    dot = before[0] * (after[0] / after_len) + before[1] * (after[1] / after_len) +
          before[2] * (after[2] / after_len);
    return isfinite(dot) && dot > 0.05;
}

/**
 * @brief Reject a collapse that would create an unordered duplicate triangle.
 *
 * Any new duplicate must contain the surviving endpoint, so only the current `a` and `b`
 * incident lists need inspection. Both lists are simulated before canonical comparison, covering
 * duplicates against existing `a` faces and between two transformed `b` faces.
 *
 * @param cx Active simplification context.
 * @param face Transformed `b`-incident face being validated.
 * @param a Surviving endpoint.
 * @param b Removed endpoint.
 * @param simulated Precomputed surviving indices for @p face.
 * @return Non-zero when no other surviving/simulated local face has the same vertex set.
 */
static int simp_simulated_face_unique(
    const simp_ctx_t *cx, uint32_t face, uint32_t a, uint32_t b, const uint32_t simulated[3]) {
    uint32_t canonical[3] = {simulated[0], simulated[1], simulated[2]};
    simp_sort_triangle(canonical);
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t endpoint = pass == 0 ? a : b;
        for (int32_t link = cx->vert_face_head[endpoint]; link >= 0; link = cx->face_links[link]) {
            uint32_t other_face = (uint32_t)link / 3u;
            uint32_t other[3];
            if (other_face == face || !simp_simulate_face(cx, other_face, a, b, other))
                continue;
            simp_sort_triangle(other);
            if (canonical[0] == other[0] && canonical[1] == other[1] && canonical[2] == other[2])
                return 0;
        }
    }
    return 1;
}

/**
 * @brief Validate topology, boundary class, geometry, and uniqueness for one collapse.
 * @param cx Mutable simplification context.
 * @param a Surviving endpoint candidate.
 * @param b Removed endpoint candidate.
 * @return Non-zero only when applying `b -> a` preserves every simplifier invariant.
 */
static int simp_collapse_is_legal(simp_ctx_t *cx, uint32_t a, uint32_t b) {
    /* Boundary locking: the removed endpoint may never sit on a classified
     * boundary, so every open, UV-seam, and material-seam polyline keeps its
     * exact vertex set (subset placement never moves the survivor). Checked
     * live rather than precomputed because face retirement can open new
     * boundaries mid-run. */
    if (cx->lock_boundaries && simp_vertex_is_classified_boundary(cx, b))
        return 0;
    if (!simp_link_condition_holds(cx, a, b))
        return 0;
    for (int32_t link = cx->vert_face_head[b]; link >= 0; link = cx->face_links[link]) {
        uint32_t face = (uint32_t)link / 3u;
        uint32_t simulated[3];
        if (!simp_face_is_live(cx, face) || simp_face_contains(cx, face, a))
            continue;
        if (!simp_simulate_face(cx, face, a, b, simulated) ||
            !simp_simulated_face_geometry_valid(cx, face, simulated) ||
            !simp_simulated_face_unique(cx, face, a, b, simulated))
            return 0;
    }
    return 1;
}

/**
 * @brief Validate private submesh ranges and expand them to one material slot per source face.
 *
 * Expansion keeps collapse-time boundary classification and output-range compaction simple. Ranges
 * must be ascending, non-overlapping, triangle aligned, inside the validated index buffer, and use
 * non-negative material slots. Uncovered triangles retain slot `-1`.
 *
 * @param cx Mutable context with `tri_count` initialized.
 * @param mesh Valid source mesh.
 * @param source_index_count Validated source index count.
 * @return Non-zero on success; zero for corrupt metadata or allocation failure.
 */
static int simp_prepare_material_slots(simp_ctx_t *cx,
                                       const rt_mesh3d *mesh,
                                       uint32_t source_index_count) {
    uint32_t previous_end = 0;
    if (!cx || !mesh || cx->tri_count == 0)
        return 0;
    cx->tri_material_slots = (int32_t *)malloc((size_t)cx->tri_count * sizeof(int32_t));
    if (!cx->tri_material_slots)
        return 0;
    for (uint32_t face = 0; face < cx->tri_count; ++face)
        cx->tri_material_slots[face] = -1;
    if (mesh->submesh_range_count == 0)
        return 1;
    if (!mesh->submesh_ranges || mesh->submesh_range_capacity < mesh->submesh_range_count)
        return 0;
    cx->has_submesh_ranges = 1;
    for (uint32_t range_index = 0; range_index < mesh->submesh_range_count; ++range_index) {
        const rt_mesh3d_submesh_range *range = &mesh->submesh_ranges[range_index];
        uint64_t end = (uint64_t)range->first_index + (uint64_t)range->index_count;
        if (range->index_count == 0 || range->material_slot < 0 ||
            (range->first_index % 3u) != 0u || (range->index_count % 3u) != 0u ||
            range->first_index < previous_end || end > source_index_count)
            return 0;
        for (uint32_t index = range->first_index; index < (uint32_t)end; index += 3u)
            cx->tri_material_slots[index / 3u] = range->material_slot;
        previous_end = (uint32_t)end;
    }
    return 1;
}

/**
 * @brief Release every native allocation owned by a simplification working context.
 * @param cx Context to clear; `NULL` is accepted.
 */
static void simp_context_dispose(simp_ctx_t *cx) {
    if (!cx)
        return;
    free(cx->verts);
    free(cx->source_vertices);
    free(cx->tris);
    free(cx->tri_material_slots);
    free(cx->quadrics);
    free(cx->stamps);
    free(cx->alive);
    free(cx->vert_face_head);
    free(cx->face_links);
    free(cx->vertex_marks);
    free(cx->face_marks);
    free(cx->face_queue);
    free(cx->heap);
    memset(cx, 0, sizeof(*cx));
}

/**
 * @brief Return the target recorded on a valid simplified Mesh3D.
 * @param mesh_obj Mesh3D receiver.
 * @return Sanitized requested triangle count, or zero for null/not-run state.
 */
int64_t rt_mesh3d_get_simplify_requested_triangles(void *mesh_obj) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    return mesh && mesh->simplify_status != RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN &&
                   mesh->simplify_requested_triangles > 0
               ? mesh->simplify_requested_triangles
               : 0;
}

/**
 * @brief Return the exact triangle count recorded on a valid simplified Mesh3D.
 * @param mesh_obj Mesh3D receiver.
 * @return Achieved triangle count, or zero for null/not-run state.
 */
int64_t rt_mesh3d_get_simplify_achieved_triangles(void *mesh_obj) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    return mesh && mesh->simplify_status != RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN &&
                   mesh->simplify_achieved_triangles >= 0
               ? mesh->simplify_achieved_triangles
               : 0;
}

/**
 * @brief Return the sanitized simplification status stored on a Mesh3D.
 * @param mesh_obj Mesh3D receiver.
 * @return `NOT_RUN`, `COMPLETE`, or `PARTIAL`; corrupt values degrade to `NOT_RUN`.
 */
int64_t rt_mesh3d_get_simplify_status(void *mesh_obj) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh || (mesh->simplify_status != RT_MESH3D_SIMPLIFY_STATUS_COMPLETE &&
                  mesh->simplify_status != RT_MESH3D_SIMPLIFY_STATUS_PARTIAL))
        return RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN;
    return mesh->simplify_status;
}

/**
 * @brief Run deterministic subset-placement QEM with manifold-safe collapses.
 *
 * All output geometry, side streams, ranges, and retained animation objects are staged before the
 * new Mesh3D publishes them. Exhausting legal edges is not an error: a valid partial mesh is
 * returned with exact requested/achieved/status diagnostics. The source mesh is never mutated.
 *
 * @param mesh_obj Source Mesh3D handle.
 * @param target_triangles Requested budget, sanitized to at least one.
 * @return A new complete/partial Mesh3D, or `NULL` for invalid input/allocation failure.
 */
void *rt_mesh3d_simplify(void *mesh_obj, int64_t target_triangles) {
    return rt_mesh3d_simplify_ex(mesh_obj, target_triangles, 0, 0.0);
}

/**
 * @brief Full-control QEM entry: optional boundary locking and error ceiling.
 *
 * Shares every invariant of rt_mesh3d_simplify. @p flags may lock classified
 * boundary vertices against removal; a positive finite @p max_error_frac converts
 * to an absolute quadric-cost ceiling of `(max_error_frac * bounding_diameter)^2`
 * (each candidate's cost is the accumulated squared plane distance at the
 * surviving endpoint) and collapsing stops at the first candidate above it.
 *
 * @param mesh_obj Source Mesh3D handle.
 * @param target_triangles Requested budget, sanitized to at least one.
 * @param flags Bitwise OR of RT_MESH3D_SIMPLIFY_FLAG_* values, or zero.
 * @param max_error_frac Error ceiling as a bounding-diameter fraction; <= 0 disables.
 * @return A new complete/partial Mesh3D, or `NULL` for invalid input/allocation failure.
 */
void *rt_mesh3d_simplify_ex(void *mesh_obj,
                            int64_t target_triangles,
                            int64_t flags,
                            double max_error_frac) {
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(mesh_obj, RT_G3D_MESH3D_CLASS_ID);
    simp_ctx_t cx;
    uint32_t src_verts;
    uint32_t src_indices;
    uint32_t target_u32;
    uint32_t *remap = NULL;
    uint32_t *out_map = NULL;
    uint32_t *out_sources = NULL;
    vgfx3d_vertex_t *out_vertices = NULL;
    uint32_t *out_indices = NULL;
    double *out_positions64 = NULL;
    vgfx3d_extra_influences_t *out_extra_influences = NULL;
    int32_t *out_bone_map = NULL;
    rt_mesh3d_submesh_range *out_ranges = NULL;
    uint32_t out_range_count = 0;
    uint32_t out_vertex_count = 0;
    uint32_t out_index_count = 0;
    rt_mesh3d *out = NULL;
    void *morph_clone = NULL;

    memset(&cx, 0, sizeof(cx));
    if (!mesh) {
        rt_trap("Mesh3D.Simplify: invalid mesh");
        return NULL;
    }
    rt_mesh3d_repair_geometry_counts(mesh);
    src_verts = rt_mesh3d_safe_vertex_count(mesh);
    src_indices = rt_mesh3d_validated_index_count(mesh);
    if (src_verts == 0 || src_indices < 3) {
        rt_trap("Mesh3D.Simplify: mesh has no triangles");
        return NULL;
    }
    if (target_triangles < 1)
        target_triangles = 1;
    target_u32 = target_triangles > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)target_triangles;

    cx.source_mesh = mesh;
    cx.lock_boundaries = (flags & RT_MESH3D_SIMPLIFY_FLAG_LOCK_BOUNDARIES) != 0 ? 1 : 0;
    if (isfinite(max_error_frac) && max_error_frac > 0.0) {
        double diameter;
        double ceiling;
        rt_mesh3d_refresh_bounds(mesh);
        diameter = mesh->bsphere_radius > 0.0f ? (double)mesh->bsphere_radius * 2.0 : 1.0;
        ceiling = max_error_frac * diameter;
        cx.max_error_cap = ceiling * ceiling;
    }
    remap = simp_weld(mesh, src_verts, &cx.verts, &cx.source_vertices, &cx.vert_count);
    if (!remap)
        goto fail;
    cx.tri_count = src_indices / 3u;
    cx.tris = (uint32_t *)malloc((size_t)cx.tri_count * 3u * sizeof(uint32_t));
    cx.quadrics = (simp_quadric_t *)calloc(cx.vert_count, sizeof(simp_quadric_t));
    cx.stamps = (uint32_t *)calloc(cx.vert_count, sizeof(uint32_t));
    cx.alive = (int8_t *)malloc(cx.vert_count);
    cx.vert_face_head = (int32_t *)malloc((size_t)cx.vert_count * sizeof(int32_t));
    cx.face_links = (int32_t *)malloc((size_t)cx.tri_count * 3u * sizeof(int32_t));
    cx.vertex_marks = (uint32_t *)calloc(cx.vert_count, sizeof(uint32_t));
    cx.face_marks = (uint32_t *)calloc(cx.tri_count, sizeof(uint32_t));
    cx.face_queue = (uint32_t *)malloc((size_t)cx.tri_count * sizeof(uint32_t));
    if (!cx.tris || !cx.quadrics || !cx.stamps || !cx.alive || !cx.vert_face_head ||
        !cx.face_links || !cx.vertex_marks || !cx.face_marks || !cx.face_queue ||
        !simp_prepare_material_slots(&cx, mesh, src_indices))
        goto fail;
    memset(cx.alive, 1, cx.vert_count);
    for (uint32_t vertex = 0; vertex < cx.vert_count; ++vertex)
        cx.vert_face_head[vertex] = -1;

    for (uint32_t face = 0; face < cx.tri_count; ++face) {
        uint32_t i0 = remap[mesh->indices[(size_t)face * 3u + 0u]];
        uint32_t i1 = remap[mesh->indices[(size_t)face * 3u + 1u]];
        uint32_t i2 = remap[mesh->indices[(size_t)face * 3u + 2u]];
        cx.tris[(size_t)face * 3u + 0u] = i0;
        cx.tris[(size_t)face * 3u + 1u] = i1;
        cx.tris[(size_t)face * 3u + 2u] = i2;
        if (i0 == i1 || i1 == i2 || i0 == i2 || !simp_face_normal(&cx, face, (double[3]){0})) {
            cx.tris[(size_t)face * 3u + 0u] = UINT32_MAX;
            cx.tris[(size_t)face * 3u + 1u] = UINT32_MAX;
            cx.tris[(size_t)face * 3u + 2u] = UINT32_MAX;
            continue;
        }
        cx.live_tris++;
        simp_link_face(&cx, face);
    }
    if (cx.live_tris == 0u) {
        rt_trap("Mesh3D.Simplify: mesh has no non-degenerate triangles");
        goto fail;
    }

    /* Build face quadrics, classified-boundary penalties, and deterministic edge candidates. */
    {
        size_t desired_table_size;
        size_t table_size = 1u;
        uint64_t *edge_keys = NULL;
        uint32_t *edge_counts = NULL;
        if (!rt_alloc_count_ok_with_extra(cx.tri_count, 8u, 1u))
            goto fail;
        desired_table_size = (size_t)cx.tri_count * 8u + 1u;
        while (table_size < desired_table_size) {
            if (table_size > SIZE_MAX / 2u)
                goto fail;
            table_size <<= 1u;
        }
        if (table_size > SIZE_MAX / sizeof(uint64_t) || table_size > SIZE_MAX / sizeof(uint32_t))
            goto fail;
        edge_keys = (uint64_t *)malloc(table_size * sizeof(uint64_t));
        edge_counts = (uint32_t *)calloc(table_size, sizeof(uint32_t));
        if (!edge_keys || !edge_counts) {
            free(edge_keys);
            free(edge_counts);
            goto fail;
        }
        memset(edge_keys, 0xFF, table_size * sizeof(uint64_t));

        for (uint32_t face = 0; face < cx.tri_count; ++face) {
            double normal[3];
            double p0[3];
            simp_quadric_t q;
            double d;
            if (!simp_face_is_live(&cx, face) || !simp_face_normal(&cx, face, normal))
                continue;
            simp_vertex_position(&cx, cx.tris[(size_t)face * 3u], p0);
            d = -(normal[0] * p0[0] + normal[1] * p0[1] + normal[2] * p0[2]);
            quadric_from_plane(&q, normal[0], normal[1], normal[2], d, 1.0);
            for (int corner = 0; corner < 3; ++corner)
                quadric_add(&cx.quadrics[cx.tris[(size_t)face * 3u + (size_t)corner]], &q);
            for (int corner = 0; corner < 3; ++corner) {
                uint32_t va = cx.tris[(size_t)face * 3u + (size_t)corner];
                uint32_t vb = cx.tris[(size_t)face * 3u + (size_t)((corner + 1) % 3)];
                uint64_t key = va < vb ? ((uint64_t)va << 32) | vb : ((uint64_t)vb << 32) | va;
                size_t slot = (size_t)(((key ^ (key >> 29)) * UINT64_C(0x9E3779B97F4A7C15)) >> 40) &
                              (table_size - 1u);
                while (edge_keys[slot] != UINT64_MAX && edge_keys[slot] != key)
                    slot = (slot + 1u) & (table_size - 1u);
                edge_keys[slot] = key;
                if (edge_counts[slot] != UINT32_MAX)
                    edge_counts[slot]++;
            }
        }
        for (size_t slot = 0; slot < table_size; ++slot) {
            uint32_t va;
            uint32_t vb;
            simp_edge_info_t edge;
            if (edge_keys[slot] == UINT64_MAX)
                continue;
            va = (uint32_t)(edge_keys[slot] >> 32);
            vb = (uint32_t)edge_keys[slot];
            simp_get_edge_info(&cx, va, vb, &edge);
            if (simp_edge_is_classified_boundary(&edge)) {
                double pa[3];
                double pb[3];
                double direction[3];
                double up[3] = {0.0, 1.0, 0.0};
                double normal[3];
                double len;
                simp_vertex_position(&cx, va, pa);
                simp_vertex_position(&cx, vb, pb);
                for (int axis = 0; axis < 3; ++axis)
                    direction[axis] = pb[axis] - pa[axis];
                if (fabs(direction[1]) > fabs(direction[0]) &&
                    fabs(direction[1]) > fabs(direction[2])) {
                    up[0] = 1.0;
                    up[1] = 0.0;
                }
                normal[0] = direction[1] * up[2] - direction[2] * up[1];
                normal[1] = direction[2] * up[0] - direction[0] * up[2];
                normal[2] = direction[0] * up[1] - direction[1] * up[0];
                len = hypot(hypot(normal[0], normal[1]), normal[2]);
                if (isfinite(len) && len > 1e-12) {
                    simp_quadric_t q;
                    double d;
                    for (int axis = 0; axis < 3; ++axis)
                        normal[axis] /= len;
                    d = -(normal[0] * pa[0] + normal[1] * pa[1] + normal[2] * pa[2]);
                    quadric_from_plane(&q, normal[0], normal[1], normal[2], d, 10.0);
                    quadric_add(&cx.quadrics[va], &q);
                    quadric_add(&cx.quadrics[vb], &q);
                }
            }
        }
        for (size_t slot = 0; slot < table_size; ++slot) {
            uint32_t va;
            uint32_t vb;
            if (edge_keys[slot] == UINT64_MAX)
                continue;
            va = (uint32_t)(edge_keys[slot] >> 32);
            vb = (uint32_t)edge_keys[slot];
            simp_push_edge(&cx, va, vb);
            simp_push_edge(&cx, vb, va);
        }
        free(edge_keys);
        free(edge_counts);
    }

    while (cx.live_tris > target_u32) {
        simp_heap_entry_t entry;
        uint32_t a;
        uint32_t b;
        int32_t link;
        if (!simp_heap_pop(&cx, &entry))
            break;
        /* The heap pops in nondecreasing stored-cost order, so the first entry
         * above the ceiling proves every remaining candidate is at least as
         * expensive; stopping here yields a valid PARTIAL result. */
        if (cx.max_error_cap > 0.0 && entry.cost > cx.max_error_cap)
            break;
        a = entry.a;
        b = entry.b;
        if (a >= cx.vert_count || b >= cx.vert_count || !cx.alive[a] || !cx.alive[b] ||
            cx.stamps[a] != entry.stamp_a || cx.stamps[b] != entry.stamp_b ||
            !simp_collapse_is_legal(&cx, a, b))
            continue;

        quadric_add(&cx.quadrics[a], &cx.quadrics[b]);
        cx.alive[b] = 0;
        cx.stamps[a]++;
        cx.stamps[b]++;
        link = cx.vert_face_head[b];
        cx.vert_face_head[b] = -1;
        while (link >= 0) {
            int32_t next = cx.face_links[link];
            uint32_t face = (uint32_t)link / 3u;
            if (simp_face_is_live(&cx, face)) {
                if (simp_face_contains(&cx, face, a)) {
                    cx.tris[(size_t)face * 3u + 0u] = UINT32_MAX;
                    cx.tris[(size_t)face * 3u + 1u] = UINT32_MAX;
                    cx.tris[(size_t)face * 3u + 2u] = UINT32_MAX;
                    cx.live_tris--;
                } else {
                    for (int corner = 0; corner < 3; ++corner) {
                        if (cx.tris[(size_t)face * 3u + (size_t)corner] == b)
                            cx.tris[(size_t)face * 3u + (size_t)corner] = a;
                    }
                    cx.face_links[link] = cx.vert_face_head[a];
                    cx.vert_face_head[a] = link;
                }
            }
            link = next;
        }
        for (int32_t adjacent_link = cx.vert_face_head[a]; adjacent_link >= 0;
             adjacent_link = cx.face_links[adjacent_link]) {
            uint32_t face = (uint32_t)adjacent_link / 3u;
            if (!simp_face_is_live(&cx, face))
                continue;
            for (int corner = 0; corner < 3; ++corner) {
                uint32_t neighbor = cx.tris[(size_t)face * 3u + (size_t)corner];
                if (neighbor != a && cx.alive[neighbor]) {
                    simp_push_edge(&cx, a, neighbor);
                    simp_push_edge(&cx, neighbor, a);
                }
            }
        }
    }

    out_map = (uint32_t *)malloc((size_t)cx.vert_count * sizeof(uint32_t));
    if (!out_map)
        goto fail;
    memset(out_map, 0xFF, (size_t)cx.vert_count * sizeof(uint32_t));
    for (uint32_t face = 0; face < cx.tri_count; ++face) {
        if (!simp_face_is_live(&cx, face))
            continue;
        for (int corner = 0; corner < 3; ++corner) {
            uint32_t vertex = cx.tris[(size_t)face * 3u + (size_t)corner];
            if (out_map[vertex] == UINT32_MAX)
                out_map[vertex] = out_vertex_count++;
        }
        out_index_count += 3u;
    }
    if (out_vertex_count == 0u || out_index_count == 0u)
        goto fail;
    out_sources = (uint32_t *)malloc((size_t)out_vertex_count * sizeof(uint32_t));
    out_vertices = (vgfx3d_vertex_t *)malloc((size_t)out_vertex_count * sizeof(vgfx3d_vertex_t));
    out_indices = (uint32_t *)malloc((size_t)out_index_count * sizeof(uint32_t));
    if (!out_sources || !out_vertices || !out_indices)
        goto fail;
    if (mesh->positions64) {
        if (!rt_alloc_count_ok(out_vertex_count, 3u * sizeof(double)))
            goto fail;
        out_positions64 = (double *)malloc((size_t)out_vertex_count * 3u * sizeof(double));
        if (!out_positions64)
            goto fail;
    }
    if (mesh->extra_influences) {
        out_extra_influences = (vgfx3d_extra_influences_t *)malloc(
            (size_t)out_vertex_count * sizeof(vgfx3d_extra_influences_t));
        if (!out_extra_influences)
            goto fail;
    }
    if (mesh->bone_map && mesh->bone_count > 0 && mesh->bone_count <= VGFX3D_MAX_BONES) {
        out_bone_map = (int32_t *)malloc((size_t)mesh->bone_count * sizeof(int32_t));
        if (!out_bone_map)
            goto fail;
        memcpy(out_bone_map, mesh->bone_map, (size_t)mesh->bone_count * sizeof(int32_t));
    }
    if (cx.has_submesh_ranges) {
        out_ranges = (rt_mesh3d_submesh_range *)malloc((size_t)cx.live_tris *
                                                       sizeof(rt_mesh3d_submesh_range));
        if (!out_ranges)
            goto fail;
    }

    for (uint32_t vertex = 0; vertex < cx.vert_count; ++vertex) {
        uint32_t output_vertex;
        uint32_t source_vertex;
        if (out_map[vertex] == UINT32_MAX)
            continue;
        output_vertex = out_map[vertex];
        source_vertex = cx.source_vertices[vertex];
        out_sources[output_vertex] = source_vertex;
        out_vertices[output_vertex] = cx.verts[vertex];
        if (out_positions64) {
            memcpy(&out_positions64[(size_t)output_vertex * 3u],
                   &mesh->positions64[(size_t)source_vertex * 3u],
                   3u * sizeof(double));
        }
        if (out_extra_influences)
            out_extra_influences[output_vertex] = mesh->extra_influences[source_vertex];
    }
    {
        uint32_t cursor = 0;
        for (uint32_t face = 0; face < cx.tri_count; ++face) {
            int32_t material_slot;
            if (!simp_face_is_live(&cx, face))
                continue;
            out_indices[cursor + 0u] = out_map[cx.tris[(size_t)face * 3u + 0u]];
            out_indices[cursor + 1u] = out_map[cx.tris[(size_t)face * 3u + 1u]];
            out_indices[cursor + 2u] = out_map[cx.tris[(size_t)face * 3u + 2u]];
            material_slot = cx.tri_material_slots[face];
            if (out_ranges && material_slot >= 0) {
                if (out_range_count > 0u &&
                    out_ranges[out_range_count - 1u].material_slot == material_slot &&
                    out_ranges[out_range_count - 1u].first_index +
                            out_ranges[out_range_count - 1u].index_count ==
                        cursor) {
                    out_ranges[out_range_count - 1u].index_count += 3u;
                } else {
                    out_ranges[out_range_count].first_index = cursor;
                    out_ranges[out_range_count].index_count = 3u;
                    out_ranges[out_range_count].material_slot = material_slot;
                    out_range_count++;
                }
            }
            cursor += 3u;
        }
        if (cursor != out_index_count)
            goto fail;
    }
    if (out_range_count == 0u) {
        free(out_ranges);
        out_ranges = NULL;
    }

    if (mesh->skeleton_ref && !rt_g3d_has_class(mesh->skeleton_ref, RT_G3D_SKELETON3D_CLASS_ID))
        goto fail;
    if (mesh->morph_targets_ref) {
        if (!rt_g3d_has_class(mesh->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID))
            goto fail;
        morph_clone =
            rt_morphtarget3d_clone_remapped(mesh->morph_targets_ref, out_sources, out_vertex_count);
        if (!morph_clone)
            goto fail;
    }

    out = (rt_mesh3d *)rt_mesh3d_new_empty_storage();
    if (!out)
        goto fail;
    out->vertices = out_vertices;
    out_vertices = NULL;
    out->vertex_count = out_vertex_count;
    out->vertex_capacity = out_vertex_count;
    out->indices = out_indices;
    out_indices = NULL;
    out->index_count = out_index_count;
    out->index_capacity = out_index_count;
    out->positions64 = out_positions64;
    out_positions64 = NULL;
    out->extra_influences = out_extra_influences;
    out_extra_influences = NULL;
    out->bone_map = out_bone_map;
    out_bone_map = NULL;
    out->bone_count =
        mesh->bone_count > 0 && mesh->bone_count <= VGFX3D_MAX_BONES ? mesh->bone_count : 0;
    out->submesh_ranges = out_ranges;
    out_ranges = NULL;
    out->submesh_range_count = out_range_count;
    out->submesh_range_capacity = out_range_count;
    out->compact_streams = mesh->compact_streams ? 1 : 0;
    if (mesh->skeleton_ref) {
        rt_obj_retain_maybe(mesh->skeleton_ref);
        out->skeleton_ref = mesh->skeleton_ref;
    }
    out->morph_targets_ref = morph_clone;
    morph_clone = NULL;
    out->bounds_dirty = 1;
    rt_mesh3d_touch_geometry(out);
    out->resident = mesh->resident ? 1 : 0;
    out->simplify_requested_triangles = target_triangles;
    out->simplify_achieved_triangles = (int64_t)(out_index_count / 3u);
    out->simplify_status = out->simplify_achieved_triangles <= target_triangles
                               ? RT_MESH3D_SIMPLIFY_STATUS_COMPLETE
                               : RT_MESH3D_SIMPLIFY_STATUS_PARTIAL;
    if (mesh->tangents_ready) {
        out->tangents_ready = 1;
        out->tangent_revision = out->geometry_revision;
    }
    rt_mesh3d_refresh_bounds(out);

    free(out_sources);
    free(out_map);
    free(remap);
    simp_context_dispose(&cx);
    return out;

fail:
    if (morph_clone && rt_obj_release_check0(morph_clone))
        rt_obj_free(morph_clone);
    if (out && rt_obj_release_check0(out))
        rt_obj_free(out);
    free(out_ranges);
    free(out_bone_map);
    free(out_extra_influences);
    free(out_positions64);
    free(out_indices);
    free(out_vertices);
    free(out_sources);
    free(out_map);
    free(remap);
    simp_context_dispose(&cx);
    return NULL;
}

/// @brief One-call LOD-chain generator feeding the existing AddLOD/SetAutoLOD
///   machinery: each successive level retains another @p ratio fraction of the
///   source triangles; distance
///   thresholds derive from the mesh's bounding radius.
/// @details Clamps @p levels to one through four and substitutes 0.4 for an invalid
///          ratio. Simplification stops on failure, each successful LOD is retained by
///          the node, and automatic LOD selection is enabled afterward.
/// @param node_obj SceneNode3D receiver whose current mesh is the LOD source.
/// @param levels Requested number of generated LOD levels.
/// @param ratio Fraction of the preceding triangle budget retained at each level.
void rt_scene_node3d_generate_lods(void *node_obj, int64_t levels, double ratio) {
    rt_scene_node3d *node =
        (rt_scene_node3d *)rt_g3d_checked_or_null(node_obj, RT_G3D_SCENENODE3D_CLASS_ID);
    if (!node)
        return;
    if (!node->mesh) {
        rt_trap("SceneNode.GenerateLODs: node has no mesh");
        return;
    }
    if (levels < 1)
        levels = 1;
    if (levels > 4)
        levels = 4;
    if (!isfinite(ratio) || ratio <= 0.0 || ratio >= 1.0)
        ratio = 0.4;
    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(node->mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh)
        return;
    rt_mesh3d_repair_geometry_counts(mesh);
    uint32_t base_tris = rt_mesh3d_validated_index_count(mesh) / 3u;
    if (base_tris < 8)
        return; /* nothing worth decimating */
    rt_mesh3d_refresh_bounds(mesh);
    double radius = mesh->bsphere_radius > 0.0f ? (double)mesh->bsphere_radius : 1.0;
    double target = (double)base_tris;
    double distance = radius * 12.0;
    for (int64_t k = 0; k < levels; k++) {
        target *= ratio;
        int64_t tri_target = (int64_t)target;
        if (tri_target < 2)
            tri_target = 2;
        void *lod = rt_mesh3d_simplify(node->mesh, tri_target);
        if (!lod)
            break;
        rt_scene_node3d_add_lod(node_obj, distance, lod);
        /* add_lod retains the mesh; drop the construction reference. */
        if (rt_obj_release_check0(lod))
            rt_obj_free(lod);
        distance *= 2.0;
    }
    rt_scene_node3d_set_auto_lod(node_obj, 1, 3.0);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
