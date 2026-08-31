//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_blendtree3d.c
// Purpose: Parametric 1D/2D BlendTree3D controller layered over AnimBlend3D —
//   maps a continuous (x, y) parameter onto per-clip animation blend weights.
//
// Key invariants:
//   - At most RT_BLENDTREE3D_MAX_SAMPLES (16) animation samples per tree.
//   - 1D trees blend the two samples bracketing param_x; 2D trees use
//     Delaunay/barycentric freeform weighting by default, with legacy
//     inverse-distance-squared weighting available explicitly.
//   - A parameter landing exactly on a sample snaps fully to it (1D shares
//     weight equally among ties; legacy 2D takes the first exact match).
//   - Weights are recomputed eagerly after sample/parameter changes and updates;
//     an identical SetParam is an O(1) no-op.
//
// Ownership/Lifetime:
//   - BlendTree3D is GC-managed; it owns the underlying AnimBlend3D and the
//     finalizer releases it. Samples are stored inline (no per-sample alloc).
//
// Links: rt_blendtree3d.h, rt_skeleton3d.h (AnimBlend3D backend)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements one- and two-dimensional parametric animation blending.
/// @details BlendTree3D owns an AnimBlend3D backend bound to one Skeleton3D.
///          It maps up to sixteen parameter-space samples to backend state
///          weights using bracketing interpolation in one dimension or either
///          Delaunay/barycentric freeform blending or legacy inverse-distance
///          weighting in two dimensions.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_blendtree3d.h"

#include "rt_g3d_ref_slots.h"
#include "rt_graphics3d_ids.h"
#include "rt_skeleton3d.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Maximum number of sample-to-animation mappings stored inline.
#define RT_BLENDTREE3D_MAX_SAMPLES 16
/// Absolute clamp applied to public parameter coordinates.
#define RT_BLENDTREE3D_PARAM_ABS_MAX 1000000.0
/// Delaunay of at most 16 points has fewer than 30 final triangles; the larger
/// bound also accommodates transient Bowyer-Watson cavity triangles.
#define RT_BLENDTREE3D_MAX_TRIS 64

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);

/// @brief One parameter-space sample: its (x, y) coordinate and the AnimBlend3D
///        state index it drives.
typedef struct {
    /// Sanitized horizontal coordinate.
    double x;
    /// Sanitized vertical coordinate; ignored by one-dimensional weighting.
    double y;
    /// Corresponding state index in the owned AnimBlend3D.
    int64_t blend_index;
} rt_blend_tree3d_sample;

/// @brief BlendTree3D state: the owned AnimBlend3D, dimensionality (1 or 2), the
///        current parameter, and the inline fixed-capacity sample table.
typedef struct {
    /// Runtime object header / virtual table slot.
    void *vptr;
    /// Owned AnimBlend3D backend.
    void *blend;
    /// Tree dimensionality, always 1 or 2.
    int32_t dimensions;
    /// Logical number of initialized inline samples.
    int32_t sample_count;
    /// Current sanitized horizontal parameter.
    double param_x;
    /// Current sanitized vertical parameter.
    double param_y;
    /// Fixed-capacity parameter-to-backend-state table.
    rt_blend_tree3d_sample samples[RT_BLENDTREE3D_MAX_SAMPLES];
    /// Two-dimensional mode: zero for freeform Delaunay/barycentric; one for
    /// legacy inverse-distance-squared weighting.
    int32_t blend_mode_2d;
    /// Cached triples of sample indexes forming the freeform triangulation.
    int32_t tris[RT_BLENDTREE3D_MAX_TRIS * 3];
    /// Number of initialized triangles in @ref tris.
    int32_t tri_count;
    /// Nonzero when sample changes require a triangulation rebuild.
    /// A clean zero-triangle cache marks a degenerate layout that falls back
    /// to legacy weighting.
    int8_t tris_dirty;
    /// Cached boundary-edge endpoint pairs derived with the triangulation.
    int32_t hull_edges[RT_BLENDTREE3D_MAX_TRIS * 3 * 2];
    /// Number of initialized endpoint pairs in `hull_edges`.
    int32_t hull_edge_count;
} rt_blend_tree3d;

/// @brief Validate @p obj as a BlendTree3D handle and return its typed pointer (NULL on mismatch).
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed tree pointer, or `NULL` on class/liveness mismatch.
static rt_blend_tree3d *blend_tree3d_checked(void *obj) {
    return (rt_blend_tree3d *)rt_g3d_checked_or_null(obj, RT_G3D_BLENDTREE3D_CLASS_ID);
}

/// @brief Return true when the owned backend still points at a live AnimBlend3D.
/// @param[in] tree Blend tree whose private backend slot to validate.
/// @return Nonzero only when both the tree and correctly typed backend exist.
static int blend_tree3d_blend_valid(const rt_blend_tree3d *tree) {
    return tree && rt_g3d_has_class(tree->blend, RT_G3D_ANIMBLEND3D_CLASS_ID);
}

/// @brief Release a GC-managed reference when this drop is the last one.
/// @param[in,out] obj Runtime object whose local owning reference to release;
///                    `NULL` is ignored.
static void blend_tree3d_release_local(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Release the owned AnimBlend3D only if the private slot still has that class.
/// @details A non-null wrong-class value is cleared as an unowned corrupt slot
///          instead of being released through the expected backend path.
/// @param[in,out] slot Address of the owned backend slot.
static void blend_tree3d_release_blend_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_ANIMBLEND3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    rt_g3d_ref_slot_release(slot);
}

/// @brief Return @p value when finite, else 0 — sanitizes parameter inputs.
/// @details Finite values outside the supported parameter range saturate to
///          `±RT_BLENDTREE3D_PARAM_ABS_MAX`.
/// @param[in] value Parameter coordinate to sanitize.
/// @return Finite, range-bounded coordinate.
static double blend_tree3d_finite_or_zero(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value > RT_BLENDTREE3D_PARAM_ABS_MAX)
        return RT_BLENDTREE3D_PARAM_ABS_MAX;
    if (value < -RT_BLENDTREE3D_PARAM_ABS_MAX)
        return -RT_BLENDTREE3D_PARAM_ABS_MAX;
    return value;
}

/// @brief Number of blend-tree samples safe to use: clamped to RT_BLENDTREE3D_MAX_SAMPLES and
///   truncated at the first sample whose blend_index falls outside the blender's state range.
/// @param[in] tree Blend tree and backend metadata to inspect.
/// @return Safe readable prefix length, or zero for missing/invalid storage.
static int32_t blend_tree3d_safe_sample_count(const rt_blend_tree3d *tree) {
    int32_t limit;
    int32_t count = 0;
    int64_t blend_state_count;
    if (!tree || tree->sample_count <= 0)
        return 0;
    if (!blend_tree3d_blend_valid(tree))
        return 0;
    blend_state_count = rt_anim_blend3d_state_count(tree->blend);
    if (blend_state_count <= 0)
        return 0;
    limit = tree->sample_count < RT_BLENDTREE3D_MAX_SAMPLES ? tree->sample_count
                                                            : RT_BLENDTREE3D_MAX_SAMPLES;
    while (count < limit && tree->samples[count].blend_index >= 0 &&
           tree->samples[count].blend_index < blend_state_count)
        count++;
    return count;
}

/// @brief Repair blend-tree metadata, parameters, samples, and cached triangulation.
/// @details A wrong-class backend reference is cleared without releasing it.
/// @param[in,out] tree Tree whose backend slot and sample count to repair.
/// @return Nonzero when observable weighting state or cache metadata changed.
static int blend_tree3d_repair_sample_count(rt_blend_tree3d *tree) {
    int changed = 0;
    int triangulation_changed = 0;
    int32_t safe_count;
    if (!tree)
        return 0;
    if (!blend_tree3d_blend_valid(tree)) {
        blend_tree3d_release_blend_ref(&tree->blend);
        tree->sample_count = 0;
        tree->tri_count = 0;
        tree->hull_edge_count = 0;
        tree->tris_dirty = 1;
        return 1;
    }
    if (tree->dimensions != 1 && tree->dimensions != 2) {
        tree->dimensions = 1;
        changed = 1;
        triangulation_changed = 1;
    }
    safe_count = blend_tree3d_safe_sample_count(tree);
    if (tree->sample_count != safe_count) {
        tree->sample_count = safe_count;
        changed = 1;
        triangulation_changed = 1;
    }
    {
        double param_x = blend_tree3d_finite_or_zero(tree->param_x);
        double param_y = blend_tree3d_finite_or_zero(tree->param_y);
        if (tree->param_x != param_x || tree->param_y != param_y)
            changed = 1;
        tree->param_x = param_x;
        tree->param_y = param_y;
    }
    if (tree->blend_mode_2d != 0 && tree->blend_mode_2d != 1) {
        tree->blend_mode_2d = 0;
        changed = 1;
    }
    for (int32_t i = 0; i < tree->sample_count; i++) {
        double x = blend_tree3d_finite_or_zero(tree->samples[i].x);
        double y = blend_tree3d_finite_or_zero(tree->samples[i].y);
        if (tree->samples[i].x != x || tree->samples[i].y != y) {
            tree->samples[i].x = x;
            tree->samples[i].y = y;
            changed = 1;
            triangulation_changed = 1;
        }
    }
    if (tree->tris_dirty != 0 && tree->tris_dirty != 1) {
        tree->tris_dirty = 1;
        changed = 1;
        triangulation_changed = 1;
    }
    if (tree->tri_count < 0 || tree->tri_count > RT_BLENDTREE3D_MAX_TRIS) {
        tree->tri_count = 0;
        tree->tris_dirty = 1;
        changed = 1;
        triangulation_changed = 1;
    }
    if (triangulation_changed) {
        tree->tri_count = 0;
        tree->hull_edge_count = 0;
        tree->tris_dirty = 1;
    } else if (!tree->tris_dirty) {
        if ((tree->tri_count > 0 && tree->hull_edge_count <= 0) || tree->hull_edge_count < 0 ||
            tree->hull_edge_count > RT_BLENDTREE3D_MAX_TRIS * 3) {
            tree->tri_count = 0;
            tree->hull_edge_count = 0;
            tree->tris_dirty = 1;
            changed = 1;
        }
        for (int32_t t = 0; !tree->tris_dirty && t < tree->tri_count; t++) {
            int32_t a = tree->tris[t * 3];
            int32_t b = tree->tris[t * 3 + 1];
            int32_t c = tree->tris[t * 3 + 2];
            if (a < 0 || b < 0 || c < 0 || a >= tree->sample_count || b >= tree->sample_count ||
                c >= tree->sample_count || a == b || b == c || a == c) {
                tree->tri_count = 0;
                tree->tris_dirty = 1;
                changed = 1;
            }
        }
        for (int32_t edge = 0; !tree->tris_dirty && edge < tree->hull_edge_count; ++edge) {
            int32_t a = tree->hull_edges[edge * 2];
            int32_t b = tree->hull_edges[edge * 2 + 1];
            if (a < 0 || b < 0 || a >= tree->sample_count || b >= tree->sample_count || a == b) {
                tree->tri_count = 0;
                tree->hull_edge_count = 0;
                tree->tris_dirty = 1;
                changed = 1;
            }
        }
    }
    return changed;
}

/// @brief Zero every sample's blend weight so a fresh weighting can be written.
/// @param[in,out] tree Tree whose valid backend state weights to clear.
static void blend_tree3d_clear_weights(rt_blend_tree3d *tree) {
    int32_t sample_count;
    if (!tree || !blend_tree3d_blend_valid(tree))
        return;
    sample_count = blend_tree3d_safe_sample_count(tree);
    for (int32_t i = 0; i < sample_count; i++)
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[i].blend_index, 0.0);
}

/// @brief Compute 1D blend weights for the current param_x and push them to AnimBlend3D.
/// @details Locates the nearest sample below (`lower`) and above (`upper`) param_x and
///          linearly interpolates their two weights by the fractional position between
///          them. Samples within 1e-9 of param_x are treated as exact and share the full
///          weight equally; when param_x falls outside the sample range, the single
///          bracketing sample receives weight 1.0.
/// @param[in,out] tree One-dimensional tree whose backend weights to replace.
static void blend_tree3d_apply_1d(rt_blend_tree3d *tree) {
    int32_t lower = -1;
    int32_t upper = -1;
    int32_t exact_count = 0;
    int32_t sample_count;
    double x;
    sample_count = blend_tree3d_safe_sample_count(tree);
    if (!tree || !blend_tree3d_blend_valid(tree) || sample_count <= 0)
        return;
    x = blend_tree3d_finite_or_zero(tree->param_x);
    blend_tree3d_clear_weights(tree);
    if (sample_count == 1) {
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[0].blend_index, 1.0);
        return;
    }
    for (int32_t i = 0; i < sample_count; i++) {
        double sx = tree->samples[i].x;
        if (!isfinite(sx))
            continue;
        if (fabs(sx - x) <= 1e-9) {
            exact_count++;
            continue;
        }
        if (sx < x && (lower < 0 || sx > tree->samples[lower].x))
            lower = i;
        if (sx > x && (upper < 0 || sx < tree->samples[upper].x))
            upper = i;
    }
    if (exact_count > 0) {
        double w = 1.0 / (double)exact_count;
        for (int32_t i = 0; i < sample_count; i++) {
            if (fabs(tree->samples[i].x - x) <= 1e-9)
                rt_anim_blend3d_set_weight(tree->blend, tree->samples[i].blend_index, w);
        }
        return;
    }
    if (lower < 0 && upper < 0) {
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[0].blend_index, 1.0);
        return;
    }
    if (lower < 0) {
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[upper].blend_index, 1.0);
        return;
    }
    if (upper < 0) {
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[lower].blend_index, 1.0);
        return;
    }
    {
        double span = tree->samples[upper].x - tree->samples[lower].x;
        double t = span > 1e-12 ? (x - tree->samples[lower].x) / span : 0.0;
        if (t < 0.0)
            t = 0.0;
        if (t > 1.0)
            t = 1.0;
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[lower].blend_index, 1.0 - t);
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[upper].blend_index, t);
    }
}

/// @brief Compute 2D blend weights for (param_x, param_y) and push them to AnimBlend3D.
/// @details Uses normalized inverse-distance-squared weighting: each sample contributes
///          1/d² of its parameter-space distance, then all weights are normalized to sum
///          to 1. A parameter landing on a sample (d² ≤ 1e-12) snaps fully to it; a
///          degenerate total (non-finite or near zero) snaps to the nearest
///          geometrically valid sample.
/// @param[in,out] tree Two-dimensional tree whose backend weights to replace.
static void blend_tree3d_apply_2d(rt_blend_tree3d *tree) {
    double raw[RT_BLENDTREE3D_MAX_SAMPLES];
    double total = 0.0;
    int32_t exact = -1;
    int32_t nearest = 0;
    double nearest_d2 = DBL_MAX;
    int32_t sample_count = blend_tree3d_safe_sample_count(tree);
    if (!tree || !blend_tree3d_blend_valid(tree) || sample_count <= 0)
        return;
    blend_tree3d_clear_weights(tree);
    if (sample_count == 1) {
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[0].blend_index, 1.0);
        return;
    }
    for (int32_t i = 0; i < sample_count; i++) {
        double dx = tree->param_x - tree->samples[i].x;
        double dy = tree->param_y - tree->samples[i].y;
        double d2 = dx * dx + dy * dy;
        raw[i] = 0.0;
        if (!isfinite(d2))
            continue;
        if (d2 < nearest_d2) {
            nearest_d2 = d2;
            nearest = i;
        }
        if (d2 <= 1e-12) {
            exact = i;
            break;
        }
        raw[i] = 1.0 / d2;
        total += raw[i];
    }
    if (exact >= 0) {
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[exact].blend_index, 1.0);
        return;
    }
    if (total <= DBL_MIN || !isfinite(total)) {
        /* Degenerate inverse-distance weighting (every sample astronomically far
         * or non-finite): snap to the geometrically nearest sample rather than
         * blindly defaulting to sample 0, which could be the farthest one. */
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[nearest].blend_index, 1.0);
        return;
    }
    for (int32_t i = 0; i < sample_count; i++)
        rt_anim_blend3d_set_weight(tree->blend, tree->samples[i].blend_index, raw[i] / total);
}

//===----------------------------------------------------------------------===//
// Freeform-directional 2D blending (Delaunay + barycentric)
//===----------------------------------------------------------------------===//

/// @brief Twice the signed area of triangle (a, b, c) in parameter space.
/// @param[in] ax First vertex X coordinate.
/// @param[in] ay First vertex Y coordinate.
/// @param[in] bx Second vertex X coordinate.
/// @param[in] by Second vertex Y coordinate.
/// @param[in] cx Third vertex X coordinate.
/// @param[in] cy Third vertex Y coordinate.
/// @return Positive for counter-clockwise orientation, negative for clockwise,
///         or zero for collinearity.
static double blend_tree3d_signed_area2(
    double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/// @brief Rebuild the Delaunay triangulation of the tree's sample points
///        (Bowyer–Watson with a super-triangle). Leaves tri_count == 0 for
///        degenerate (collinear / duplicate-heavy) layouts.
/// @details Every rebuild clears the dirty flag first. A working-capacity
///          overflow also leaves zero final triangles so weighting can fall
///          back safely.
/// @param[in,out] tree Tree whose cached triangle triples to replace.
static void blend_tree3d_triangulate(rt_blend_tree3d *tree) {
    tree->tri_count = 0;
    tree->hull_edge_count = 0;
    tree->tris_dirty = 0;
    int32_t n = blend_tree3d_safe_sample_count(tree);
    if (n < 3)
        return;

    /* Working points: samples plus a super-triangle enclosing everything. */
    double px[RT_BLENDTREE3D_MAX_SAMPLES + 3];
    double py[RT_BLENDTREE3D_MAX_SAMPLES + 3];
    double mnx = DBL_MAX, mny = DBL_MAX, mxx = -DBL_MAX, mxy = -DBL_MAX;
    for (int32_t i = 0; i < n; i++) {
        px[i] = blend_tree3d_finite_or_zero(tree->samples[i].x);
        py[i] = blend_tree3d_finite_or_zero(tree->samples[i].y);
        if (px[i] < mnx)
            mnx = px[i];
        if (py[i] < mny)
            mny = py[i];
        if (px[i] > mxx)
            mxx = px[i];
        if (py[i] > mxy)
            mxy = py[i];
    }
    double span = (mxx - mnx) > (mxy - mny) ? (mxx - mnx) : (mxy - mny);
    if (span < 1e-12)
        return; /* all samples coincide */
    double cx = (mnx + mxx) * 0.5;
    double cy = (mny + mxy) * 0.5;
    int32_t s0 = n, s1 = n + 1, s2 = n + 2;
    px[s0] = cx - 20.0 * span;
    py[s0] = cy - 10.0 * span;
    px[s1] = cx + 20.0 * span;
    py[s1] = cy - 10.0 * span;
    px[s2] = cx;
    py[s2] = cy + 20.0 * span;

    /* Triangle scratch (indices into px/py). */
    int32_t tv[RT_BLENDTREE3D_MAX_TRIS * 3];
    int8_t alive[RT_BLENDTREE3D_MAX_TRIS];
    int32_t tri_count = 0;
    tv[0] = s0;
    tv[1] = s1;
    tv[2] = s2;
    alive[0] = 1;
    tri_count = 1;

    /* Cavity boundary edge buffer. */
    int32_t ea[RT_BLENDTREE3D_MAX_TRIS * 3];
    int32_t eb[RT_BLENDTREE3D_MAX_TRIS * 3];

    for (int32_t ip = 0; ip < n; ip++) {
        int32_t edge_count = 0;
        for (int32_t t = 0; t < tri_count; t++) {
            if (!alive[t])
                continue;
            int32_t a = tv[t * 3], b = tv[t * 3 + 1], c = tv[t * 3 + 2];
            double area2 = blend_tree3d_signed_area2(px[a], py[a], px[b], py[b], px[c], py[c]);
            if (area2 < 0.0) {
                int32_t tmp = b;
                b = c;
                c = tmp;
                area2 = -area2;
            }
            if (area2 < 1e-18)
                continue; /* sliver: skip circumcircle math */
            double adx = px[a] - px[ip], ady = py[a] - py[ip];
            double bdx = px[b] - px[ip], bdy = py[b] - py[ip];
            double cdx = px[c] - px[ip], cdy = py[c] - py[ip];
            double det = (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) -
                         (bdx * bdx + bdy * bdy) * (adx * cdy - cdx * ady) +
                         (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
            if (det <= 0.0)
                continue; /* point outside this triangle's circumcircle */
            /* Triangle dies; its edges join the cavity boundary (an edge
             * appearing twice is interior and cancels). */
            alive[t] = 0;
            int32_t evs[3][2] = {{tv[t * 3], tv[t * 3 + 1]},
                                 {tv[t * 3 + 1], tv[t * 3 + 2]},
                                 {tv[t * 3 + 2], tv[t * 3]}};
            for (int32_t e = 0; e < 3; e++) {
                int found = -1;
                for (int32_t k = 0; k < edge_count; k++) {
                    if ((ea[k] == evs[e][0] && eb[k] == evs[e][1]) ||
                        (ea[k] == evs[e][1] && eb[k] == evs[e][0])) {
                        found = k;
                        break;
                    }
                }
                if (found >= 0) {
                    ea[found] = ea[edge_count - 1];
                    eb[found] = eb[edge_count - 1];
                    edge_count--;
                } else if (edge_count < RT_BLENDTREE3D_MAX_TRIS * 3) {
                    ea[edge_count] = evs[e][0];
                    eb[edge_count] = evs[e][1];
                    edge_count++;
                }
            }
        }
        /* Compact dead triangles, then fan new ones from the cavity edges. */
        int32_t w = 0;
        for (int32_t t = 0; t < tri_count; t++) {
            if (!alive[t])
                continue;
            tv[w * 3] = tv[t * 3];
            tv[w * 3 + 1] = tv[t * 3 + 1];
            tv[w * 3 + 2] = tv[t * 3 + 2];
            alive[w] = 1;
            w++;
        }
        tri_count = w;
        for (int32_t k = 0; k < edge_count; k++) {
            if (tri_count >= RT_BLENDTREE3D_MAX_TRIS)
                return; /* overflow: stay degenerate (falls back to IDW) */
            tv[tri_count * 3] = ea[k];
            tv[tri_count * 3 + 1] = eb[k];
            tv[tri_count * 3 + 2] = ip;
            alive[tri_count] = 1;
            tri_count++;
        }
    }

    /* Keep only triangles free of super-triangle vertices. */
    for (int32_t t = 0; t < tri_count; t++) {
        int32_t a = tv[t * 3], b = tv[t * 3 + 1], c = tv[t * 3 + 2];
        if (a >= n || b >= n || c >= n)
            continue;
        tree->tris[tree->tri_count * 3] = a;
        tree->tris[tree->tri_count * 3 + 1] = b;
        tree->tris[tree->tri_count * 3 + 2] = c;
        tree->tri_count++;
    }
    /* Cache the boundary once. Adding every triangle edge and cancelling its reversed duplicate
     * leaves exactly the convex-hull edges; parameter updates can then project in O(hull) rather
     * than rediscovering adjacency with an O(triangle squared) scan every frame. */
    for (int32_t t = 0; t < tree->tri_count; ++t) {
        for (int32_t edge = 0; edge < 3; ++edge) {
            int32_t a = tree->tris[t * 3 + edge];
            int32_t b = tree->tris[t * 3 + (edge + 1) % 3];
            int32_t found = -1;
            for (int32_t cached = 0; cached < tree->hull_edge_count; ++cached) {
                int32_t ca = tree->hull_edges[cached * 2];
                int32_t cb = tree->hull_edges[cached * 2 + 1];
                if ((ca == a && cb == b) || (ca == b && cb == a)) {
                    found = cached;
                    break;
                }
            }
            if (found >= 0) {
                --tree->hull_edge_count;
                tree->hull_edges[found * 2] = tree->hull_edges[tree->hull_edge_count * 2];
                tree->hull_edges[found * 2 + 1] = tree->hull_edges[tree->hull_edge_count * 2 + 1];
            } else if (tree->hull_edge_count < RT_BLENDTREE3D_MAX_TRIS * 3) {
                tree->hull_edges[tree->hull_edge_count * 2] = a;
                tree->hull_edges[tree->hull_edge_count * 2 + 1] = b;
                ++tree->hull_edge_count;
            }
        }
    }
}

/// @brief Freeform-directional 2D weighting: barycentric inside the containing
///        Delaunay triangle (at most 3 non-zero weights), nearest-hull-edge
///        projection outside the hull (2 weights). Degenerate triangulations
///        fall back to the legacy inverse-distance weighting.
/// @param[in,out] tree Two-dimensional tree whose cached triangulation and
///                     backend weights to update.
static void blend_tree3d_apply_2d_freeform(rt_blend_tree3d *tree) {
    int32_t sample_count = blend_tree3d_safe_sample_count(tree);
    if (!tree || !blend_tree3d_blend_valid(tree) || sample_count <= 0)
        return;
    if (tree->tris_dirty)
        blend_tree3d_triangulate(tree);
    if (tree->tri_count <= 0) {
        blend_tree3d_apply_2d(tree); /* collinear layout: legacy weighting */
        return;
    }

    double x = blend_tree3d_finite_or_zero(tree->param_x);
    double y = blend_tree3d_finite_or_zero(tree->param_y);

    /* Interior: barycentric weights over the containing triangle. */
    for (int32_t t = 0; t < tree->tri_count; t++) {
        int32_t a = tree->tris[t * 3], b = tree->tris[t * 3 + 1], c = tree->tris[t * 3 + 2];
        double ax = tree->samples[a].x, ay = tree->samples[a].y;
        double bx = tree->samples[b].x, by = tree->samples[b].y;
        double cx = tree->samples[c].x, cy = tree->samples[c].y;
        double area2 = blend_tree3d_signed_area2(ax, ay, bx, by, cx, cy);
        if (fabs(area2) < 1e-18)
            continue;
        double wa = blend_tree3d_signed_area2(x, y, bx, by, cx, cy) / area2;
        double wb = blend_tree3d_signed_area2(ax, ay, x, y, cx, cy) / area2;
        double wc = 1.0 - wa - wb;
        const double tol = -1e-9;
        if (wa >= tol && wb >= tol && wc >= tol) {
            double sum;
            wa = wa < 0.0 ? 0.0 : wa;
            wb = wb < 0.0 ? 0.0 : wb;
            wc = wc < 0.0 ? 0.0 : wc;
            sum = wa + wb + wc;
            if (!isfinite(sum) || sum <= DBL_MIN)
                continue;
            wa /= sum;
            wb /= sum;
            wc /= sum;
            blend_tree3d_clear_weights(tree);
            rt_anim_blend3d_set_weight(tree->blend, tree->samples[a].blend_index, wa);
            rt_anim_blend3d_set_weight(tree->blend, tree->samples[b].blend_index, wb);
            rt_anim_blend3d_set_weight(tree->blend, tree->samples[c].blend_index, wc);
            return;
        }
    }

    /* Exterior: project onto the nearest hull (boundary) edge and lerp its
     * two clips. A boundary edge belongs to exactly one triangle. */
    double best_d2 = DBL_MAX;
    int32_t best_a = tree->hull_edge_count > 0 ? tree->hull_edges[0] : tree->tris[0];
    int32_t best_b = tree->hull_edge_count > 0 ? tree->hull_edges[1] : tree->tris[1];
    double best_t = 0.0;
    for (int32_t edge = 0; edge < tree->hull_edge_count; ++edge) {
        int32_t a = tree->hull_edges[edge * 2];
        int32_t b = tree->hull_edges[edge * 2 + 1];
        double ax = tree->samples[a].x, ay = tree->samples[a].y;
        double bx = tree->samples[b].x, by = tree->samples[b].y;
        double ex = bx - ax, ey = by - ay;
        double len2 = ex * ex + ey * ey;
        double proj = len2 > 1e-18 ? ((x - ax) * ex + (y - ay) * ey) / len2 : 0.0;
        if (proj < 0.0)
            proj = 0.0;
        if (proj > 1.0)
            proj = 1.0;
        double qx = ax + ex * proj, qy = ay + ey * proj;
        double d2 = (x - qx) * (x - qx) + (y - qy) * (y - qy);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_a = a;
            best_b = b;
            best_t = proj;
        }
    }
    blend_tree3d_clear_weights(tree);
    rt_anim_blend3d_set_weight(tree->blend, tree->samples[best_a].blend_index, 1.0 - best_t);
    rt_anim_blend3d_set_weight(tree->blend, tree->samples[best_b].blend_index, best_t);
}

/// @brief Dispatch to the 1D or 2D weighting routine based on the tree's dimensionality.
/// @details Two-dimensional mode one selects legacy inverse-distance weighting;
///          every other two-dimensional mode selects freeform weighting.
/// @param[in,out] tree Tree whose current parameters to apply.
static void blend_tree3d_apply_weights(rt_blend_tree3d *tree) {
    if (!tree)
        return;
    if (tree->dimensions == 2) {
        if (tree->blend_mode_2d == 1)
            blend_tree3d_apply_2d(tree);
        else
            blend_tree3d_apply_2d_freeform(tree);
    } else {
        blend_tree3d_apply_1d(tree);
    }
}

/// @brief GC finalizer: release the owned AnimBlend3D backend.
/// @param[in,out] obj BlendTree3D storage being finalized; `NULL` is ignored.
static void blend_tree3d_finalize(void *obj) {
    rt_blend_tree3d *tree = (rt_blend_tree3d *)obj;
    if (!tree)
        return;
    blend_tree3d_release_blend_ref(&tree->blend);
}

/// @brief Shared constructor for 1D/2D trees: wrap a new AnimBlend3D bound to @p skeleton.
/// @details @p dimensions is clamped to 1 or 2. Returns NULL if @p skeleton is not a
///          Skeleton3D or if either the AnimBlend3D or the tree allocation fails.
/// @param[in] skeleton Borrowed Skeleton3D handle passed to the backend
///                     constructor.
/// @param[in] dimensions Requested dimensionality; exactly two selects 2D and
///                       every other value selects 1D.
/// @return New GC-managed BlendTree3D, or `NULL` for an invalid skeleton or
///         allocation/backend-construction failure.
static void *blend_tree3d_new(void *skeleton, int32_t dimensions) {
    rt_blend_tree3d *tree;
    void *blend;
    if (!rt_g3d_has_class(skeleton, RT_G3D_SKELETON3D_CLASS_ID))
        return NULL;
    blend = rt_anim_blend3d_new(skeleton);
    if (!blend)
        return NULL;
    tree = (rt_blend_tree3d *)rt_obj_new_i64(RT_G3D_BLENDTREE3D_CLASS_ID,
                                             (int64_t)sizeof(rt_blend_tree3d));
    if (!tree) {
        blend_tree3d_release_local(blend);
        return NULL;
    }
    memset(tree, 0, sizeof(*tree));
    tree->dimensions = dimensions == 2 ? 2 : 1;
    tree->blend = blend;
    for (int32_t i = 0; i < RT_BLENDTREE3D_MAX_SAMPLES; i++)
        tree->samples[i].blend_index = -1;
    rt_obj_set_finalizer(tree, blend_tree3d_finalize);
    return tree;
}

/// @brief Create a 1D blend tree bound to @p skeleton.
/// @param[in] skeleton Borrowed Skeleton3D handle retained indirectly by the
///                     owned AnimBlend3D.
/// @return New GC-managed one-dimensional BlendTree3D, or `NULL` on invalid
///         input or allocation failure.
void *rt_blend_tree3d_new_1d(void *skeleton) {
    return blend_tree3d_new(skeleton, 1);
}

/// @brief Create a 2D blend tree bound to @p skeleton.
/// @param[in] skeleton Borrowed Skeleton3D handle retained indirectly by the
///                     owned AnimBlend3D.
/// @return New GC-managed two-dimensional BlendTree3D using freeform mode, or
///         `NULL` on invalid input or allocation failure.
void *rt_blend_tree3d_new_2d(void *skeleton) {
    return blend_tree3d_new(skeleton, 2);
}

/// @brief Register an animation sample at parameter coordinate (x, y) and reblend.
/// @details Non-finite coordinates are sanitized to 0. Returns the new sample's index,
///          or -1 if the handle/animation is invalid or the sample table is full.
///          Finite coordinates saturate to the supported parameter range.
///          The backend validates skeleton compatibility and retains the clip
///          when it creates the corresponding blend state.
/// @param[in,out] obj BlendTree3D to extend.
/// @param[in] animation Borrowed Animation3D sample clip.
/// @param[in] x Horizontal parameter coordinate.
/// @param[in] y Vertical coordinate; stored but ignored by a 1D tree.
/// @return Newly appended zero-based sample index, or `-1` for invalid input,
///         incompatibility, capacity exhaustion, or backend failure.
int64_t rt_blend_tree3d_add_sample(void *obj, void *animation, double x, double y) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    int64_t blend_index;
    if (!tree || !blend_tree3d_blend_valid(tree) ||
        !rt_g3d_has_class(animation, RT_G3D_ANIMATION3D_CLASS_ID))
        return -1;
    blend_tree3d_repair_sample_count(tree);
    if (tree->sample_count >= RT_BLENDTREE3D_MAX_SAMPLES)
        return -1;
    blend_index = rt_anim_blend3d_add_state(tree->blend, NULL, animation);
    if (blend_index < 0)
        return -1;
    tree->samples[tree->sample_count].x = blend_tree3d_finite_or_zero(x);
    tree->samples[tree->sample_count].y = blend_tree3d_finite_or_zero(y);
    tree->samples[tree->sample_count].blend_index = blend_index;
    tree->sample_count++;
    tree->tris_dirty = 1;
    blend_tree3d_apply_weights(tree);
    return tree->sample_count - 1;
}

/// @brief Set the current blend parameters (sanitized to finite) and recompute weights.
/// @param[in,out] obj BlendTree3D to configure.
/// @param[in] x Horizontal parameter coordinate, sanitized and range-clamped.
/// @param[in] y Vertical parameter coordinate, sanitized and range-clamped.
void rt_blend_tree3d_set_param(void *obj, double x, double y) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    double sanitized_x;
    double sanitized_y;
    int repaired;
    if (!tree)
        return;
    repaired = blend_tree3d_repair_sample_count(tree);
    sanitized_x = blend_tree3d_finite_or_zero(x);
    sanitized_y = blend_tree3d_finite_or_zero(y);
    if (!repaired && tree->param_x == sanitized_x && tree->param_y == sanitized_y)
        return;
    tree->param_x = sanitized_x;
    tree->param_y = sanitized_y;
    blend_tree3d_apply_weights(tree);
}

/// @brief Recompute sample weights and advance the underlying AnimBlend3D by @p dt seconds.
/// @details Negative or non-finite elapsed time becomes zero, which still
///          refreshes weights and evaluates the backend without advancing its
///          clocks.
/// @param[in,out] obj BlendTree3D to evaluate.
/// @param[in] dt Requested elapsed time in seconds.
void rt_blend_tree3d_update(void *obj, double dt) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    if (!tree || !blend_tree3d_blend_valid(tree))
        return;
    blend_tree3d_repair_sample_count(tree);
    if (!isfinite(dt) || dt < 0.0)
        dt = 0.0;
    blend_tree3d_apply_weights(tree);
    rt_anim_blend3d_update(tree->blend, dt);
}

/// @brief Number of samples currently registered (0 for an invalid handle).
/// @param[in] obj BlendTree3D to inspect.
/// @return Sanitized readable sample count.
int64_t rt_blend_tree3d_get_sample_count(void *obj) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    if (!tree)
        return 0;
    if (blend_tree3d_repair_sample_count(tree))
        blend_tree3d_apply_weights(tree);
    return tree->sample_count;
}

/// @brief Borrow the underlying AnimBlend3D handle (not retained; NULL if invalid).
/// @param[in] obj BlendTree3D to inspect.
/// @return Borrowed backend handle, or `NULL`; the caller must not release it.
void *rt_blend_tree3d_get_blend(void *obj) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    return blend_tree3d_blend_valid(tree) ? tree->blend : NULL;
}

/// @brief Select the 2D weighting mode: 0 = freeform-directional (Delaunay +
///        barycentric, the default — at most 3 non-zero weights, hull-edge
///        projection outside), 1 = legacy inverse-distance-squared (kept for
///        content authored against the old soft blending). No effect on 1D
///        trees. Weights are recomputed immediately.
/// @param[in,out] obj BlendTree3D to configure.
/// @param[in] mode One for legacy inverse-distance weighting; every other
///                 value selects freeform weighting.
void rt_blend_tree3d_set_blend_mode(void *obj, int64_t mode) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    int32_t sanitized;
    int repaired;
    if (!tree)
        return;
    repaired = blend_tree3d_repair_sample_count(tree);
    if (tree->dimensions != 2) {
        if (repaired)
            blend_tree3d_apply_weights(tree);
        return;
    }
    sanitized = mode == 1 ? 1 : 0;
    if (!repaired && tree->blend_mode_2d == sanitized)
        return;
    tree->blend_mode_2d = sanitized;
    blend_tree3d_apply_weights(tree);
}

/// @brief Current 2D weighting mode (0 = freeform, 1 = legacy IDW).
/// @param[in] obj BlendTree3D to inspect.
/// @return Stored normalized mode, or zero for an invalid handle.
int64_t rt_blend_tree3d_get_blend_mode(void *obj) {
    rt_blend_tree3d *tree = blend_tree3d_checked(obj);
    if (!tree)
        return 0;
    if (blend_tree3d_repair_sample_count(tree))
        blend_tree3d_apply_weights(tree);
    return tree->dimensions == 2 ? tree->blend_mode_2d : 0;
}

#else
typedef int rt_blendtree3d_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
