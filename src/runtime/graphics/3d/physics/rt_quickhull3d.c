//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/physics/rt_quickhull3d.c
// Purpose: From-scratch 3D quickhull for physics collider reduction. Classic
//          conflict-list formulation: build an initial tetrahedron from
//          extreme points, then repeatedly lift the farthest conflict point,
//          carve the visible face set, and stitch new faces across the
//          horizon loop.
// Key invariants:
//   - Face normals always point away from the hull interior centroid.
//   - The horizon is a closed edge loop; new faces link neighbor-consistent.
//   - Epsilons scale with the input's bounding-box diagonal so tiny and huge
//     clouds behave identically.
// Ownership/Lifetime:
//   - All working memory is heap-allocated and freed before return; outputs
//     are transferred to the caller (free()).
// Links: rt_quickhull3d.h, rt_collider3d.c, src/tests/unit/test_quickhull3d.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements dependency-free three-dimensional QuickHull and point-cloud reduction.
///
/// Hull construction maintains triangular faces with per-face outside-point conflict lists,
/// removes each visible region around the globally farthest point, and stitches a new fan
/// across the horizon. A separate farthest-point sampler preserves axis extrema when callers
/// must reduce large clouds before hull construction.

#include "rt_quickhull3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @brief Transient triangular hull face with conflict list and adjacency.
typedef struct {
    int32_t v[3];        ///< Vertex indices into the input cloud.
    int32_t neighbor[3]; ///< Face index across edge (v[i] -> v[(i+1)%3]).
    double normal[3];    ///< Outward unit-ish normal (not normalized).
    double offset;       ///< Plane offset: dot(normal, x) == offset on-plane.
    int32_t *conflicts;  ///< Input point indices strictly outside this face.
    int32_t conflict_count;
    int32_t conflict_cap;
    int32_t farthest_conflict;
    double farthest_distance;
    int8_t alive;
} qh_face;

typedef struct {
    const double *pts;
    int32_t n;
    qh_face *faces;
    int32_t face_count;
    int32_t face_cap;
    double eps;
} qh_ctx;

/// @brief Returns a borrowed point from the context's packed xyz coordinate array.
/// @param ctx Borrowed hull context.
/// @param i Zero-based point index.
/// @return Pointer to the first of the point's three coordinates.
static const double *qh_p(const qh_ctx *ctx, int32_t i) {
    return ctx->pts + (size_t)i * 3u;
}

/// @brief Subtracts one three-dimensional vector from another.
/// @param a Borrowed minuend vector.
/// @param b Borrowed subtrahend vector.
/// @param[out] out Three-element destination for `a - b`.
static void qh_sub(const double *a, const double *b, double *out) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

/// @brief Computes the right-handed cross product of two three-dimensional vectors.
/// @param a Borrowed first vector.
/// @param b Borrowed second vector.
/// @param[out] out Three-element destination for `a` crossed with `b`.
static void qh_cross(const double *a, const double *b, double *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// @brief Computes the Euclidean dot product of two three-dimensional vectors.
/// @param a Borrowed first vector.
/// @param b Borrowed second vector.
/// @return Scalar dot product.
static double qh_dot(const double *a, const double *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// @brief Signed distance (unnormalized) of point i from face f's plane.
/// @param ctx Borrowed context supplying point coordinates.
/// @param f Borrowed face containing its outward plane equation.
/// @param i Zero-based point index.
/// @return Positive for a point outside the face, negative inside, and zero on its plane, scaled
///         by the face normal's magnitude.
static double qh_face_dist(const qh_ctx *ctx, const qh_face *f, int32_t i) {
    return qh_dot(f->normal, qh_p(ctx, i)) - f->offset;
}

static void qh_face_refresh_farthest(const qh_ctx *ctx, qh_face *face) {
    face->farthest_conflict = -1;
    face->farthest_distance = ctx->eps;
    for (int32_t index = 0; index < face->conflict_count; ++index) {
        const int32_t point = face->conflicts[index];
        const double distance = qh_face_dist(ctx, face, point);
        if (distance > face->farthest_distance) {
            face->farthest_distance = distance;
            face->farthest_conflict = point;
        }
    }
}

/// @brief Compute a face's plane from its current winding (no reorientation).
/// @param ctx Borrowed context supplying vertex coordinates.
/// @param[out] f Face whose unnormalized normal and plane offset are recomputed.
static void qh_face_plane_raw(qh_ctx *ctx, qh_face *f) {
    double e1[3];
    double e2[3];
    qh_sub(qh_p(ctx, f->v[1]), qh_p(ctx, f->v[0]), e1);
    qh_sub(qh_p(ctx, f->v[2]), qh_p(ctx, f->v[0]), e2);
    qh_cross(e1, e2, f->normal);
    f->offset = qh_dot(f->normal, qh_p(ctx, f->v[0]));
}

/// @brief Compute a face's plane, flipping the winding so the normal points
///        away from `interior`. ONLY safe before adjacency is assigned (the
///        initial tetrahedron): flipping reorders the edges that neighbor
///        slots key on. Horizon faces inherit outward winding from the
///        visible face they replace and must use qh_face_plane_raw.
/// @param ctx Borrowed context supplying vertex coordinates.
/// @param[in,out] f Face whose winding and plane equation may be reversed.
/// @param interior Borrowed point known to lie inside the initial tetrahedron.
static void qh_face_plane(qh_ctx *ctx, qh_face *f, const double *interior) {
    qh_face_plane_raw(ctx, f);
    if (qh_dot(f->normal, interior) - f->offset > 0.0) {
        int32_t tmp = f->v[1];
        f->v[1] = f->v[2];
        f->v[2] = tmp;
        f->normal[0] = -f->normal[0];
        f->normal[1] = -f->normal[1];
        f->normal[2] = -f->normal[2];
        f->offset = -f->offset;
    }
}

/// @brief Appends one outside-point index to a face's growable conflict list.
/// @param[in,out] f Face whose owned conflict allocation may grow geometrically.
/// @param point Input point index to append.
/// @return `1` on success, or `0` when list allocation fails.
static int qh_face_push_conflict(qh_face *f, int32_t point) {
    if (f->conflict_count >= f->conflict_cap) {
        int32_t cap = f->conflict_cap > 0 ? f->conflict_cap * 2 : 8;
        int32_t *grown = (int32_t *)realloc(f->conflicts, (size_t)cap * sizeof(int32_t));
        if (!grown)
            return 0;
        f->conflicts = grown;
        f->conflict_cap = cap;
    }
    f->conflicts[f->conflict_count++] = point;
    return 1;
}

/// @brief Allocates and initializes the next face slot in a hull context.
/// @param[in,out] ctx Context whose owned face array may grow geometrically.
/// @return Index of a new alive zeroed face, or `-1` on allocation failure.
static int32_t qh_alloc_face(qh_ctx *ctx) {
    if (ctx->face_count >= ctx->face_cap) {
        int32_t cap = ctx->face_cap > 0 ? ctx->face_cap * 2 : 64;
        qh_face *grown = (qh_face *)realloc(ctx->faces, (size_t)cap * sizeof(qh_face));
        if (!grown)
            return -1;
        ctx->faces = grown;
        ctx->face_cap = cap;
    }
    memset(&ctx->faces[ctx->face_count], 0, sizeof(qh_face));
    ctx->faces[ctx->face_count].alive = 1;
    ctx->faces[ctx->face_count].farthest_conflict = -1;
    return ctx->face_count++;
}

/// @brief Releases every conflict list and the face array owned by a hull context.
/// @param[in,out] ctx Context to clear after either successful output or failure.
static void qh_free_all(qh_ctx *ctx) {
    for (int32_t i = 0; i < ctx->face_count; i++)
        free(ctx->faces[i].conflicts);
    free(ctx->faces);
    ctx->faces = NULL;
    ctx->face_count = 0;
    ctx->face_cap = 0;
}

/// @brief Locate the neighbor slot on face `f` that points to face `old`.
/// @param f Borrowed triangular face.
/// @param old Neighbor face index to locate.
/// @return Edge-slot index in `[0, 2]`, or `-1` when @p old is not adjacent.
static int32_t qh_neighbor_slot(const qh_face *f, int32_t old) {
    for (int32_t i = 0; i < 3; i++) {
        if (f->neighbor[i] == old)
            return i;
    }
    return -1;
}

static uint32_t qh_vertex_hash(int32_t vertex) {
    uint32_t value = (uint32_t)vertex;
    value ^= value >> 16u;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15u;
    value *= UINT32_C(0x846ca68b);
    return value ^ (value >> 16u);
}

/// @brief Choose the four initial tetrahedron corners from the extremes.
/// @details Selects the most separated axis-extreme pair, the point farthest from that line, and
///          finally the point farthest from the resulting plane. Context-scaled epsilon tests
///          reject coincident, collinear, and coplanar clouds.
/// @param ctx Borrowed initialized context containing at least four finite points.
/// @param[out] out Four-element destination for distinct input point indices.
/// @return 1 on success, 0 when the cloud is degenerate (flat/collinear).
static int qh_initial_tetra(qh_ctx *ctx, int32_t out[4]) {
    int32_t min_axis[3] = {0, 0, 0};
    int32_t max_axis[3] = {0, 0, 0};
    for (int32_t i = 1; i < ctx->n; i++) {
        for (int32_t a = 0; a < 3; a++) {
            if (qh_p(ctx, i)[a] < qh_p(ctx, min_axis[a])[a])
                min_axis[a] = i;
            if (qh_p(ctx, i)[a] > qh_p(ctx, max_axis[a])[a])
                max_axis[a] = i;
        }
    }

    /* Most separated extreme pair -> hull edge. */
    int32_t e0 = min_axis[0];
    int32_t e1 = max_axis[0];
    double best = -1.0;
    int32_t candidates[6] = {
        min_axis[0], max_axis[0], min_axis[1], max_axis[1], min_axis[2], max_axis[2]};
    for (int32_t i = 0; i < 6; i++) {
        for (int32_t j = i + 1; j < 6; j++) {
            double d[3];
            qh_sub(qh_p(ctx, candidates[i]), qh_p(ctx, candidates[j]), d);
            double len = qh_dot(d, d);
            if (len > best) {
                best = len;
                e0 = candidates[i];
                e1 = candidates[j];
            }
        }
    }
    if (best <= ctx->eps * ctx->eps)
        return 0;

    /* Farthest point from the line (e0, e1) -> triangle. */
    double dir[3];
    qh_sub(qh_p(ctx, e1), qh_p(ctx, e0), dir);
    int32_t e2 = -1;
    best = ctx->eps * ctx->eps;
    for (int32_t i = 0; i < ctx->n; i++) {
        double rel[3];
        double crossed[3];
        qh_sub(qh_p(ctx, i), qh_p(ctx, e0), rel);
        qh_cross(dir, rel, crossed);
        double dist2 = qh_dot(crossed, crossed) / qh_dot(dir, dir);
        if (dist2 > best) {
            best = dist2;
            e2 = i;
        }
    }
    if (e2 < 0)
        return 0;

    /* Farthest point from the triangle plane -> tetrahedron apex. */
    double u[3];
    double v[3];
    double plane_n[3];
    qh_sub(qh_p(ctx, e1), qh_p(ctx, e0), u);
    qh_sub(qh_p(ctx, e2), qh_p(ctx, e0), v);
    qh_cross(u, v, plane_n);
    double plane_off = qh_dot(plane_n, qh_p(ctx, e0));
    int32_t e3 = -1;
    best = ctx->eps * sqrt(qh_dot(plane_n, plane_n));
    for (int32_t i = 0; i < ctx->n; i++) {
        double dist = fabs(qh_dot(plane_n, qh_p(ctx, i)) - plane_off);
        if (dist > best) {
            best = dist;
            e3 = i;
        }
    }
    if (e3 < 0)
        return 0;

    out[0] = e0;
    out[1] = e1;
    out[2] = e2;
    out[3] = e3;
    return 1;
}

/// @brief Builds an outward-wound triangular convex hull from a finite point cloud.
/// @details A scale-adjusted epsilon makes degeneracy and outside-face tests consistent across
///          coordinate magnitudes. The builder transfers compact vertex and optional triangle
///          arrays only after the entire hull succeeds; all output pointers and counts are reset
///          before validation, and every transient allocation is released on failure.
/// @param points Borrowed packed xyz coordinates for @p point_count input points.
/// @param point_count Number of input points; at least four are required.
/// @param[out] out_vertices Required destination for a caller-owned packed xyz allocation.
/// @param[out] out_vertex_count Required destination for the compact hull vertex count.
/// @param[out] out_indices Optional destination for a caller-owned packed triangle-index
///        allocation. When omitted, the hull is still constructed and vertices are returned.
/// @param[out] out_index_count Optional destination for the emitted scalar index count; set only
///        when @p out_indices is requested.
/// @return `1` when a nondegenerate hull is produced; `0` for invalid or non-finite input,
///         coincident/collinear/coplanar geometry, pathological growth, or allocation failure.
/// @note The caller releases successful @p out_vertices and @p out_indices allocations with
///       `free()`.
int rt_quickhull3d_build(const double *points,
                         int32_t point_count,
                         double **out_vertices,
                         int32_t *out_vertex_count,
                         int32_t **out_indices,
                         int32_t *out_index_count) {
    if (out_vertices)
        *out_vertices = NULL;
    if (out_vertex_count)
        *out_vertex_count = 0;
    if (out_indices)
        *out_indices = NULL;
    if (out_index_count)
        *out_index_count = 0;
    if (!points || point_count < 4 || !out_vertices || !out_vertex_count)
        return 0;
    if (point_count > INT32_MAX / 8 - 64)
        return 0; /* keeps point_count*3 and the 8*n+64 face guard in int32 range */
    double coordinate_scale = 0.0;
    for (int64_t i = 0; i < (int64_t)point_count * 3; i++) {
        if (!isfinite(points[i]))
            return 0;
        if (fabs(points[i]) > coordinate_scale)
            coordinate_scale = fabs(points[i]);
    }
    if (!(coordinate_scale > 0.0) || (size_t)point_count > SIZE_MAX / (3u * sizeof(double)))
        return 0;
    double *scaled_points = (double *)malloc((size_t)point_count * 3u * sizeof(double));
    if (!scaled_points)
        return 0;
    for (int64_t i = 0; i < (int64_t)point_count * 3; ++i)
        scaled_points[i] = points[i] / coordinate_scale;

    qh_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.pts = scaled_points;
    ctx.n = point_count;

    /* Epsilon scaled by the bounding-box diagonal. */
    double mn[3] = {scaled_points[0], scaled_points[1], scaled_points[2]};
    double mx[3] = {scaled_points[0], scaled_points[1], scaled_points[2]};
    for (int32_t i = 1; i < point_count; i++) {
        for (int32_t a = 0; a < 3; a++) {
            double c = qh_p(&ctx, i)[a];
            if (c < mn[a])
                mn[a] = c;
            if (c > mx[a])
                mx[a] = c;
        }
    }
    double diag[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
    ctx.eps = 1e-9 * sqrt(qh_dot(diag, diag)) + 1e-12;

    int32_t tet[4];
    if (!qh_initial_tetra(&ctx, tet)) {
        free(scaled_points);
        return 0;
    }

    double interior[3] = {
        qh_p(&ctx, tet[0])[0] * 0.25 + qh_p(&ctx, tet[1])[0] * 0.25 + qh_p(&ctx, tet[2])[0] * 0.25 +
            qh_p(&ctx, tet[3])[0] * 0.25,
        qh_p(&ctx, tet[0])[1] * 0.25 + qh_p(&ctx, tet[1])[1] * 0.25 + qh_p(&ctx, tet[2])[1] * 0.25 +
            qh_p(&ctx, tet[3])[1] * 0.25,
        qh_p(&ctx, tet[0])[2] * 0.25 + qh_p(&ctx, tet[1])[2] * 0.25 + qh_p(&ctx, tet[2])[2] * 0.25 +
            qh_p(&ctx, tet[3])[2] * 0.25,
    };

    /* Four tetra faces; adjacency filled by brute force below. */
    static const int32_t tet_faces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (int32_t fi = 0; fi < 4; fi++) {
        int32_t idx = qh_alloc_face(&ctx);
        if (idx < 0) {
            qh_free_all(&ctx);
            free(scaled_points);
            return 0;
        }
        qh_face *f = &ctx.faces[idx];
        f->v[0] = tet[tet_faces[fi][0]];
        f->v[1] = tet[tet_faces[fi][1]];
        f->v[2] = tet[tet_faces[fi][2]];
        qh_face_plane(&ctx, f, interior);
    }
    /* Adjacency: two tetra faces share an edge iff they share two vertices. */
    for (int32_t a = 0; a < 4; a++) {
        for (int32_t e = 0; e < 3; e++) {
            int32_t va = ctx.faces[a].v[e];
            int32_t vb = ctx.faces[a].v[(e + 1) % 3];
            for (int32_t b = 0; b < 4; b++) {
                if (a == b)
                    continue;
                for (int32_t e2 = 0; e2 < 3; e2++) {
                    int32_t wa = ctx.faces[b].v[e2];
                    int32_t wb = ctx.faces[b].v[(e2 + 1) % 3];
                    if ((va == wa && vb == wb) || (va == wb && vb == wa))
                        ctx.faces[a].neighbor[e] = b;
                }
            }
        }
    }

    /* Initial conflict assignment. */
    for (int32_t i = 0; i < point_count; i++) {
        if (i == tet[0] || i == tet[1] || i == tet[2] || i == tet[3])
            continue;
        for (int32_t fi = 0; fi < ctx.face_count; fi++) {
            if (qh_face_dist(&ctx, &ctx.faces[fi], i) > ctx.eps) {
                if (!qh_face_push_conflict(&ctx.faces[fi], i)) {
                    qh_free_all(&ctx);
                    free(scaled_points);
                    return 0;
                }
                break;
            }
        }
    }
    for (int32_t fi = 0; fi < ctx.face_count; ++fi)
        qh_face_refresh_farthest(&ctx, &ctx.faces[fi]);

    /* Working buffers for the expansion loop. */
    int32_t *visible = (int32_t *)malloc(sizeof(int32_t) * 8);
    int32_t visible_cap = 8;
    int32_t *stack = (int32_t *)malloc(sizeof(int32_t) * 8);
    int32_t stack_cap = 8;
    int8_t *mark = (int8_t *)calloc((size_t)ctx.face_cap, 1);
    int32_t mark_cap = ctx.face_cap;
    int32_t *horizon_slots = NULL;
    int32_t horizon_cap = 0;
    if (!visible || !stack || !mark)
        goto fail;

    for (;;) {
        /* Pick the face whose cached farthest conflict point is globally farthest. */
        int32_t best_face = -1;
        int32_t best_point = -1;
        double best_dist = ctx.eps;
        for (int32_t fi = 0; fi < ctx.face_count; fi++) {
            qh_face *f = &ctx.faces[fi];
            if (!f->alive || f->farthest_conflict < 0)
                continue;
            if (f->farthest_distance > best_dist) {
                best_dist = f->farthest_distance;
                best_face = fi;
                best_point = f->farthest_conflict;
            }
        }
        if (best_face < 0)
            break; /* hull complete */

        /* Guard against pathological growth. */
        if (ctx.face_count > 8 * point_count + 64)
            goto fail;

        /* Flood-fill the visible face set from the seed. */
        if (ctx.face_cap > mark_cap) {
            int8_t *grown = (int8_t *)realloc(mark, (size_t)ctx.face_cap);
            if (!grown)
                goto fail;
            mark = grown;
            memset(mark + mark_cap, 0, (size_t)(ctx.face_cap - mark_cap));
            mark_cap = ctx.face_cap;
        }
        memset(mark, 0, (size_t)mark_cap);
        int32_t visible_count = 0;
        int32_t stack_count = 0;
        stack[stack_count++] = best_face;
        mark[best_face] = 1;
        while (stack_count > 0) {
            int32_t fi = stack[--stack_count];
            if (visible_count >= visible_cap) {
                int32_t cap = visible_cap * 2;
                int32_t *grown = (int32_t *)realloc(visible, sizeof(int32_t) * (size_t)cap);
                if (!grown)
                    goto fail;
                visible = grown;
                visible_cap = cap;
            }
            visible[visible_count++] = fi;
            for (int32_t e = 0; e < 3; e++) {
                int32_t nb = ctx.faces[fi].neighbor[e];
                if (nb < 0 || mark[nb] || !ctx.faces[nb].alive)
                    continue;
                if (qh_face_dist(&ctx, &ctx.faces[nb], best_point) > ctx.eps) {
                    mark[nb] = 1;
                    if (stack_count >= stack_cap) {
                        int32_t cap = stack_cap * 2;
                        int32_t *grown = (int32_t *)realloc(stack, sizeof(int32_t) * (size_t)cap);
                        if (!grown)
                            goto fail;
                        stack = grown;
                        stack_cap = cap;
                    }
                    stack[stack_count++] = nb;
                }
            }
        }

        /* Stitch new faces across every horizon edge (visible -> hidden). */
        int32_t first_new = ctx.face_count;
        for (int32_t vi = 0; vi < visible_count; vi++) {
            int32_t fi = visible[vi];
            for (int32_t e = 0; e < 3; e++) {
                int32_t nb = ctx.faces[fi].neighbor[e];
                if (nb >= 0 && mark[nb])
                    continue; /* interior edge of the visible set */
                int32_t a = ctx.faces[fi].v[e];
                int32_t b = ctx.faces[fi].v[(e + 1) % 3];
                int32_t idx = qh_alloc_face(&ctx);
                if (idx < 0)
                    goto fail;
                qh_face *nf = &ctx.faces[idx];
                nf->v[0] = a;
                nf->v[1] = b;
                nf->v[2] = best_point;
                /* Horizon edge taken in the visible face's winding order plus
                 * an apex strictly above that face's plane => outward winding
                 * by construction. Never reorient (slots key on the edges). */
                qh_face_plane_raw(&ctx, nf);
                nf->neighbor[0] = nb;
                nf->neighbor[1] = -1;
                nf->neighbor[2] = -1;
                if (nb >= 0) {
                    int32_t slot = qh_neighbor_slot(&ctx.faces[nb], fi);
                    if (slot >= 0)
                        ctx.faces[nb].neighbor[slot] = idx;
                }
            }
        }
        int32_t new_count = ctx.face_count - first_new;
        /* Index each horizon edge by its starting vertex, then link the fan in linear time. */
        {
            int32_t needed = 16;
            while (needed < new_count * 2) {
                if (needed > INT32_MAX / 2)
                    goto fail;
                needed *= 2;
            }
            if (needed > horizon_cap) {
                int32_t *grown =
                    (int32_t *)realloc(horizon_slots, (size_t)needed * sizeof(int32_t));
                if (!grown)
                    goto fail;
                horizon_slots = grown;
                horizon_cap = needed;
            }
            for (int32_t slot = 0; slot < horizon_cap; ++slot)
                horizon_slots[slot] = -1;
            for (int32_t offset = 0; offset < new_count; ++offset) {
                qh_face *face = &ctx.faces[first_new + offset];
                uint32_t slot = qh_vertex_hash(face->v[0]) & (uint32_t)(horizon_cap - 1);
                while (horizon_slots[slot] >= 0) {
                    if (ctx.faces[first_new + horizon_slots[slot]].v[0] == face->v[0])
                        goto fail;
                    slot = (slot + 1u) & (uint32_t)(horizon_cap - 1);
                }
                horizon_slots[slot] = offset;
            }
            for (int32_t offset = 0; offset < new_count; ++offset) {
                qh_face *face = &ctx.faces[first_new + offset];
                uint32_t slot = qh_vertex_hash(face->v[1]) & (uint32_t)(horizon_cap - 1);
                while (horizon_slots[slot] >= 0 &&
                       ctx.faces[first_new + horizon_slots[slot]].v[0] != face->v[1])
                    slot = (slot + 1u) & (uint32_t)(horizon_cap - 1);
                if (horizon_slots[slot] < 0)
                    goto fail;
                int32_t next_face = first_new + horizon_slots[slot];
                if (ctx.faces[next_face].neighbor[2] >= 0)
                    goto fail;
                face->neighbor[1] = next_face;
                ctx.faces[next_face].neighbor[2] = first_new + offset;
            }
        }

        /* Retire visible faces, redistributing their conflict points. */
        for (int32_t vi = 0; vi < visible_count; vi++) {
            qh_face *f = &ctx.faces[visible[vi]];
            f->alive = 0;
            for (int32_t c = 0; c < f->conflict_count; c++) {
                int32_t p = f->conflicts[c];
                if (p == best_point)
                    continue;
                for (int32_t nfi = first_new; nfi < ctx.face_count; nfi++) {
                    if (qh_face_dist(&ctx, &ctx.faces[nfi], p) > ctx.eps) {
                        if (!qh_face_push_conflict(&ctx.faces[nfi], p))
                            goto fail;
                        break;
                    }
                }
            }
            free(f->conflicts);
            f->conflicts = NULL;
            f->conflict_count = 0;
            f->conflict_cap = 0;
            f->farthest_conflict = -1;
        }
        for (int32_t nfi = first_new; nfi < ctx.face_count; ++nfi)
            qh_face_refresh_farthest(&ctx, &ctx.faces[nfi]);
    }

    /* Compact: remap used vertices, emit faces. */
    {
        int32_t *remap = (int32_t *)malloc(sizeof(int32_t) * (size_t)point_count);
        if (!remap)
            goto fail;
        for (int32_t i = 0; i < point_count; i++)
            remap[i] = -1;

        int32_t vert_count = 0;
        int32_t tri_count = 0;
        for (int32_t fi = 0; fi < ctx.face_count; fi++) {
            if (!ctx.faces[fi].alive)
                continue;
            tri_count++;
            for (int32_t e = 0; e < 3; e++) {
                if (remap[ctx.faces[fi].v[e]] < 0)
                    remap[ctx.faces[fi].v[e]] = vert_count++;
            }
        }
        double *verts = (double *)malloc(sizeof(double) * 3u * (size_t)vert_count);
        int32_t *indices =
            out_indices ? (int32_t *)malloc(sizeof(int32_t) * 3u * (size_t)tri_count) : NULL;
        if (!verts || (out_indices && !indices)) {
            free(remap);
            free(verts);
            free(indices);
            goto fail;
        }
        for (int32_t i = 0; i < point_count; i++) {
            if (remap[i] >= 0) {
                verts[remap[i] * 3 + 0] = points[i * 3 + 0];
                verts[remap[i] * 3 + 1] = points[i * 3 + 1];
                verts[remap[i] * 3 + 2] = points[i * 3 + 2];
            }
        }
        if (out_indices) {
            int32_t w = 0;
            for (int32_t fi = 0; fi < ctx.face_count; fi++) {
                if (!ctx.faces[fi].alive)
                    continue;
                indices[w++] = remap[ctx.faces[fi].v[0]];
                indices[w++] = remap[ctx.faces[fi].v[1]];
                indices[w++] = remap[ctx.faces[fi].v[2]];
            }
            *out_indices = indices;
            if (out_index_count)
                *out_index_count = w;
        }
        *out_vertices = verts;
        *out_vertex_count = vert_count;
        free(remap);
    }

    free(visible);
    free(stack);
    free(mark);
    free(horizon_slots);
    qh_free_all(&ctx);
    free(scaled_points);
    return 1;

fail:
    free(visible);
    free(stack);
    free(mark);
    free(horizon_slots);
    qh_free_all(&ctx);
    free(scaled_points);
    return 0;
}

/// @brief Reduces a packed point cloud with deterministic farthest-point sampling.
/// @details The six axis extrema are selected first, with duplicate indices removed, so support
///          bounds survive when capacity permits. Remaining slots greedily choose the point whose
///          nearest selected neighbor is farthest. Input order is preserved only when no reduction
///          is required.
/// @param points Borrowed packed xyz coordinates for @p point_count points.
/// @param point_count Positive input point count.
/// @param max_points Positive maximum number of points to retain.
/// @param[out] out_points Caller-owned packed xyz buffer with capacity for at least
///        `min(point_count, max_points)` points.
/// @return Number of points written, which may be smaller than @p max_points when all remaining
///         coordinates coincide; zero for invalid arguments or scratch allocation failure.
int32_t rt_quickhull3d_reduce(const double *points,
                              int32_t point_count,
                              int32_t max_points,
                              double *out_points) {
    if (!points || point_count <= 0 || max_points <= 0 || !out_points)
        return 0;
    double coordinate_scale = 0.0;
    for (int64_t index = 0; index < (int64_t)point_count * 3; ++index) {
        if (!isfinite(points[index]))
            return 0;
        if (fabs(points[index]) > coordinate_scale)
            coordinate_scale = fabs(points[index]);
    }
    if (point_count <= max_points) {
        memcpy(out_points, points, sizeof(double) * 3u * (size_t)point_count);
        return point_count;
    }
    if ((size_t)point_count > SIZE_MAX / (3u * sizeof(double)))
        return 0;
    if (!(coordinate_scale > 0.0))
        coordinate_scale = 1.0;

    int32_t *chosen = (int32_t *)malloc(sizeof(int32_t) * (size_t)max_points);
    double *best_d2 = (double *)malloc(sizeof(double) * (size_t)point_count);
    double *scaled_points = (double *)malloc(sizeof(double) * 3u * (size_t)point_count);
    if (!chosen || !best_d2 || !scaled_points) {
        free(chosen);
        free(best_d2);
        free(scaled_points);
        return 0;
    }
    for (int64_t index = 0; index < (int64_t)point_count * 3; ++index)
        scaled_points[index] = points[index] / coordinate_scale;

    /* Seed with the six axis extremes (dedup'd) to guarantee the support
     * bounds survive, then greedily add the point farthest from the set. */
    int32_t chosen_count = 0;
    for (int32_t a = 0; a < 3; a++) {
        int32_t lo = 0;
        int32_t hi = 0;
        for (int32_t i = 1; i < point_count; i++) {
            if (points[i * 3 + a] < points[lo * 3 + a])
                lo = i;
            if (points[i * 3 + a] > points[hi * 3 + a])
                hi = i;
        }
        int32_t seeds[2] = {lo, hi};
        for (int32_t s = 0; s < 2 && chosen_count < max_points; s++) {
            int dup = 0;
            for (int32_t c = 0; c < chosen_count; c++) {
                if (chosen[c] == seeds[s])
                    dup = 1;
            }
            if (!dup)
                chosen[chosen_count++] = seeds[s];
        }
    }

    for (int32_t i = 0; i < point_count; i++) {
        best_d2[i] = INFINITY;
        for (int32_t c = 0; c < chosen_count; c++) {
            double dx = scaled_points[i * 3 + 0] - scaled_points[chosen[c] * 3 + 0];
            double dy = scaled_points[i * 3 + 1] - scaled_points[chosen[c] * 3 + 1];
            double dz = scaled_points[i * 3 + 2] - scaled_points[chosen[c] * 3 + 2];
            double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2[i])
                best_d2[i] = d2;
        }
    }

    while (chosen_count < max_points) {
        int32_t far_idx = -1;
        double far_d2 = 0.0;
        for (int32_t i = 0; i < point_count; i++) {
            if (best_d2[i] > far_d2) {
                far_d2 = best_d2[i];
                far_idx = i;
            }
        }
        if (far_idx < 0 || far_d2 <= 0.0)
            break; /* every remaining point coincides with a chosen one */
        chosen[chosen_count++] = far_idx;
        for (int32_t i = 0; i < point_count; i++) {
            double dx = scaled_points[i * 3 + 0] - scaled_points[far_idx * 3 + 0];
            double dy = scaled_points[i * 3 + 1] - scaled_points[far_idx * 3 + 1];
            double dz = scaled_points[i * 3 + 2] - scaled_points[far_idx * 3 + 2];
            double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2[i])
                best_d2[i] = d2;
        }
    }

    for (int32_t c = 0; c < chosen_count; c++) {
        out_points[c * 3 + 0] = points[chosen[c] * 3 + 0];
        out_points[c * 3 + 1] = points[chosen[c] * 3 + 1];
        out_points[c * 3 + 2] = points[chosen[c] * 3 + 2];
    }
    free(chosen);
    free(best_d2);
    free(scaled_points);
    return chosen_count;
}
