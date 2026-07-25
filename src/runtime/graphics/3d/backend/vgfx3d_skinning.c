//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_skinning.c
// Purpose: CPU-side skeletal vertex skinning. Transforms each vertex's
//   position and normal by up to 4 weighted bone matrices from the palette.
//
// Key invariants:
//   - Bone indices are clamped to the 8-bit palette range used by runtime vertices.
//   - Normal matrices are precomputed once per bone when caller scratch is available.
//   - Non-finite inputs are sanitized or skipped before writing destination vertices.
// Ownership/Lifetime:
//   - Source, destination, and palette storage are caller-owned.
//   - Optional scratch storage is caller-owned and grows without per-frame frees.
// Links: vgfx3d_skinning.h, plans/3d/14-skeletal-animation.md
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_skinning.c
 * @brief Implements defensive CPU linear-blend skinning for Graphics3D vertices.
 *
 * Vertices may use four inline influences and four optional side-stream influences. Position
 * transforms use full affine bone matrices, while normals and tangents use inverse-transpose
 * matrices and are normalized after blending. Optional grow-only scratch buffers cache per-bone
 * validity and normal matrices; allocation failure falls back to equivalent per-influence work.
 */

#ifdef ZANNA_ENABLE_GRAPHICS

#include "vgfx3d_skinning.h"
#include "vgfx3d_backend_utils.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Maximum accepted individual influence weight before normalization.
#define VGFX3D_SKIN_WEIGHT_MAX 1000000.0
/// Number of bone indices representable by the vertex format's eight-bit fields.
#define VGFX3D_SKIN_INDEX_RANGE 256

/// @brief Release all owned skinning scratch buffers.
/// @param scratch Mutable caller-owned scratch state; null is a no-op. The structure remains valid
///                and reset for later reuse.
void vgfx3d_skinning_scratch_free(vgfx3d_skinning_scratch_t *scratch) {
    if (!scratch)
        return;
    free(scratch->normal_palette);
    scratch->normal_palette = NULL;
    scratch->normal_palette_capacity = 0;
    scratch->normal_palette_grow_count = 0;
    free(scratch->bone_valid);
    scratch->bone_valid = NULL;
    scratch->bone_valid_capacity = 0;
}

/// @brief Ensure @p scratch can hold one validity flag per effective bone.
/// @param scratch Mutable caller-owned grow-only scratch state.
/// @param bone_count Positive effective bone count.
/// @return Pointer to at least @p bone_count writable flags, or null for invalid input or allocation
///         failure.
static uint8_t *skin_ensure_bone_valid_scratch(vgfx3d_skinning_scratch_t *scratch,
                                               int32_t bone_count) {
    uint8_t *grown;
    if (!scratch || bone_count <= 0)
        return NULL;
    if (scratch->bone_valid && scratch->bone_valid_capacity >= bone_count)
        return scratch->bone_valid;
    grown = (uint8_t *)realloc(scratch->bone_valid, (size_t)bone_count);
    if (!grown)
        return NULL;
    scratch->bone_valid = grown;
    scratch->bone_valid_capacity = bone_count;
    return scratch->bone_valid;
}

/// @brief Clamp a bone count to the usable skinning range [0, VGFX3D_SKIN_INDEX_RANGE].
/// @param bone_count Caller-provided palette size.
/// @return Effective count in `[0, VGFX3D_SKIN_INDEX_RANGE]`.
static int32_t skin_effective_bone_count(int32_t bone_count) {
    if (bone_count <= 0)
        return 0;
    return bone_count < VGFX3D_SKIN_INDEX_RANGE ? bone_count : VGFX3D_SKIN_INDEX_RANGE;
}

/// @brief Ensure @p scratch can hold one normal matrix per effective bone.
/// @param scratch Mutable caller-owned grow-only scratch state.
/// @param bone_count Positive effective bone count.
/// @return Pointer to storage for at least `bone_count * 16` floats, or null for invalid input,
///         overflow, or allocation failure.
static float *skin_ensure_normal_palette_scratch(vgfx3d_skinning_scratch_t *scratch,
                                                 int32_t bone_count) {
    float *grown;
    if (!scratch || bone_count <= 0)
        return NULL;
    if (scratch->normal_palette && scratch->normal_palette_capacity >= bone_count)
        return scratch->normal_palette;
    if ((size_t)bone_count > SIZE_MAX / (16u * sizeof(float)))
        return NULL;
    size_t normal_palette_values = (size_t)bone_count * 16u;
    grown = (float *)realloc(scratch->normal_palette, normal_palette_values * sizeof(float));
    if (!grown)
        return NULL;
    scratch->normal_palette = grown;
    scratch->normal_palette_capacity = bone_count;
    scratch->normal_palette_grow_count++;
    return scratch->normal_palette;
}

/// @brief True only if all 16 elements of a 4×4 matrix are finite (NULL matrix returns false).
/// @param m Borrowed 16-element matrix.
/// @return Non-zero when @p m is non-null and every element is finite.
static int skin_matrix4_is_finite(const float *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return 0;
    }
    return 1;
}

/// @brief True if a 3-vector is non-NULL and all three lanes are finite.
/// @param v Borrowed three-component double-precision vector.
/// @return Non-zero when @p v is non-null and all components are finite.
static int skin_vec3_is_usable(const double v[3]) {
    return v && isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/// @brief Return @p value when finite, else @p fallback.
/// @param value Preferred value.
/// @param fallback Replacement returned when @p value is non-finite.
/// @return @p value when finite, otherwise @p fallback.
static float skin_finite_float_or(float value, float fallback) {
    return isfinite(value) ? value : fallback;
}

/// @brief Convert a bone weight to a clamped double, returning 0 for non-finite or negligible
///   (≤1e-6) weights so the caller skips them.
/// @param value Source influence weight.
/// @return Zero for a skipped influence, otherwise a positive value no greater than
///         @c VGFX3D_SKIN_WEIGHT_MAX.
static double skin_weight_or_skip(float value) {
    double w;
    if (!isfinite(value) || value <= 1e-6f)
        return 0.0;
    w = (double)value;
    return w > VGFX3D_SKIN_WEIGHT_MAX ? VGFX3D_SKIN_WEIGHT_MAX : w;
}

/// @brief Finite-guard a skinned vertex in place: scrub position/normal/tangent/UV lanes, clamp
///   bone weights to [0, max], and normalize the tangent handedness sign to ±1.
/// @param v Mutable vertex to sanitize; null is a no-op.
static void skin_sanitize_vertex(vgfx3d_vertex_t *v) {
    if (!v)
        return;
    for (int i = 0; i < 3; i++) {
        v->pos[i] = skin_finite_float_or(v->pos[i], 0.0f);
        v->normal[i] = skin_finite_float_or(v->normal[i], 0.0f);
        v->tangent[i] = skin_finite_float_or(v->tangent[i], 0.0f);
    }
    for (int i = 0; i < 2; i++) {
        v->uv[i] = skin_finite_float_or(v->uv[i], 0.0f);
        v->uv1[i] = skin_finite_float_or(v->uv1[i], v->uv[i]);
    }
    for (int i = 0; i < 4; i++) {
        if (!isfinite(v->bone_weights[i]) || v->bone_weights[i] < 0.0f)
            v->bone_weights[i] = 0.0f;
        else if (v->bone_weights[i] > (float)VGFX3D_SKIN_WEIGHT_MAX)
            v->bone_weights[i] = (float)VGFX3D_SKIN_WEIGHT_MAX;
    }
    if (!isfinite(v->tangent[3]) || v->tangent[3] == 0.0f)
        v->tangent[3] = 1.0f;
    else
        v->tangent[3] = v->tangent[3] < 0.0f ? -1.0f : 1.0f;
}

/// @brief Normalize a 3-vector and store it as floats in @p out; returns 0 (leaving @p out
///   unset) when the input is unusable or of ~zero length.
/// @param in Mutable double-precision vector normalized in place.
/// @param out Caller-owned three-float destination, written only for a usable normalized vector.
/// @return Non-zero when normalization and finite float conversion succeed, otherwise zero.
static int skin_store_normalized_vec3(double in[3], float out[3]) {
    double len2;
    double inv_len;
    if (!skin_vec3_is_usable(in) || !out)
        return 0;
    len2 = in[0] * in[0] + in[1] * in[1] + in[2] * in[2];
    if (!isfinite(len2) || len2 <= 1e-16)
        return 0;
    inv_len = 1.0 / sqrt(len2);
    in[0] *= inv_len;
    in[1] *= inv_len;
    in[2] *= inv_len;
    if (!skin_vec3_is_usable(in))
        return 0;
    out[0] = (float)in[0];
    out[1] = (float)in[1];
    out[2] = (float)in[2];
    return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]);
}

/// @brief Apply 4-influence linear blend skinning on the CPU.
/// @details For every vertex, sums `weight[i] * palette[bone_index[i]] * v` over the
///   four bone influences carried in `src[v].bone_weights` / `src[v].bone_indices`.
///   Positions take the full affine row (translation column included); normals use
///   each bone's inverse-transpose normal matrix and are re-normalized. Influences
///   with weight below 1e-6 or whose bone index exceeds `bone_count` are skipped.
///   CPU skinning enforces the engine's 256-bone palette limit because vertex
///   bone indices are stored as uint8_t and every GPU skinning path has the same
///   upper bound.
///   Non-position/normal
///   attributes are passed through by copying `src[v]` into `dst[v]` first when the
///   buffers differ (in-place skinning is supported by leaving `dst == src`).
///   Vertices with zero total weight fall through unchanged so unskinned geometry
///   is preserved for meshes that mix rigid and skinned verts.
/// @param src Source vertex array (read-only).
/// @param dst Destination vertex array; may alias `src`.
/// @param vertex_count Number of vertices to transform.
/// @param palette Row-major 4x4 matrix palette laid out as `bone_count` matrices
///   stored back-to-back (16 floats per matrix).
/// @param bone_count Upper bound on valid indices in `palette`.
/// @param scratch Optional caller-owned grow-only cache for normal matrices and bone-validity flags.
void vgfx3d_skin_vertices(const vgfx3d_vertex_t *src,
                          vgfx3d_vertex_t *dst,
                          uint32_t vertex_count,
                          const float *palette,
                          int32_t bone_count,
                          vgfx3d_skinning_scratch_t *scratch) {
    vgfx3d_skin_vertices_extra(src, dst, vertex_count, palette, bone_count, NULL, scratch);
}

/// @brief Apply CPU linear-blend skinning with up to eight influences per vertex.
/// @details The first four influences come from each @p src vertex and the optional @p extra
///          side stream supplies influences five through eight. Usable weights are normalized by
///          their accumulated sum; invalid matrices, out-of-range indices, and negligible weights
///          are skipped. Positions use affine matrices, normals and tangents use normal matrices,
///          and all other sanitized attributes pass through. In-place operation is supported.
/// @param src Borrowed source array of @p vertex_count vertices.
/// @param dst Caller-owned destination array of @p vertex_count vertices; may equal @p src.
/// @param vertex_count Number of vertices and, when present, side-stream entries to process.
/// @param palette Borrowed row-major palette containing @p bone_count consecutive 4×4 matrices.
/// @param bone_count Palette size, clamped to the 256 indices representable by vertex records.
/// @param extra Optional borrowed array containing four additional influences per vertex.
/// @param scratch Optional caller-owned grow-only cache for normal matrices and bone-validity flags.
void vgfx3d_skin_vertices_extra(const vgfx3d_vertex_t *src,
                                vgfx3d_vertex_t *dst,
                                uint32_t vertex_count,
                                const float *palette,
                                int32_t bone_count,
                                const vgfx3d_extra_influences_t *extra,
                                vgfx3d_skinning_scratch_t *scratch) {
    if (!src || !dst || vertex_count == 0)
        return;
    if (!palette || bone_count <= 0) {
        if (dst != src) {
            if ((size_t)vertex_count > SIZE_MAX / sizeof(*dst))
                return;
            memcpy(dst, src, (size_t)vertex_count * sizeof(*dst));
        }
        return;
    }
    bone_count = skin_effective_bone_count(bone_count);
    if (bone_count <= 0)
        return;

    /* A bone's normal matrix (inverse-transpose of its skinning matrix) and
     * its finiteness depend only on the bone, not the vertex, so precompute
     * BOTH once per palette rather than re-deriving them per vertex per
     * influence (the finiteness checks alone were ~128 isfinite() calls per
     * vertex). Falls back to inline per-influence computation if caller
     * scratch is unavailable or cannot grow. */
    int32_t normal_bone_count = bone_count;
    float *normal_palette = skin_ensure_normal_palette_scratch(scratch, normal_bone_count);
    uint8_t *bone_valid = skin_ensure_bone_valid_scratch(scratch, bone_count);
    if (bone_valid) {
        for (int32_t b = 0; b < bone_count; b++)
            bone_valid[b] = skin_matrix4_is_finite(&palette[(size_t)b * 16u]) ? 1 : 0;
    }
    if (normal_palette) {
        for (int32_t b = 0; b < normal_bone_count; b++) {
            if (!bone_valid || bone_valid[b])
                vgfx3d_compute_normal_matrix4(&palette[(size_t)b * 16u],
                                              &normal_palette[(size_t)b * 16u]);
            else
                memset(&normal_palette[(size_t)b * 16u], 0, 16u * sizeof(float));
        }
    }

    for (uint32_t v = 0; v < vertex_count; v++) {
        vgfx3d_vertex_t base = src[v];
        double pos[3] = {0.0, 0.0, 0.0};
        double nrm[3] = {0.0, 0.0, 0.0};
        double tan[3] = {0.0, 0.0, 0.0};
        double total_w = 0.0;
        int normal_influences = 0;
        int tangent_influences = 0;

        skin_sanitize_vertex(&base);

        for (int b = 0; b < 4; b++) {
            double w = skin_weight_or_skip(base.bone_weights[b]);
            if (w <= 0.0)
                continue;
            int idx = (int)base.bone_indices[b];
            if (idx >= bone_count)
                continue;

            const float *m = &palette[(size_t)idx * 16u];
            /* Per-bone validity precomputed above; the inline check only runs
             * on the scratch-less fallback path. */
            if (bone_valid ? !bone_valid[idx] : !skin_matrix4_is_finite(m))
                continue;
            float nm_local[16];
            const float *nm;
            int nm_known_finite = 0;
            if (normal_palette && idx < normal_bone_count) {
                nm = &normal_palette[(size_t)idx * 16u];
                nm_known_finite = bone_valid != NULL; /* zeroed slots are finite too */
            } else {
                vgfx3d_compute_normal_matrix4(m, nm_local);
                nm = nm_local;
            }
            /* pos += w * (M * src_pos) — row-major multiply */
            for (int i = 0; i < 3; i++) {
                pos[i] += w * ((double)m[i * 4 + 0] * (double)base.pos[0] +
                               (double)m[i * 4 + 1] * (double)base.pos[1] +
                               (double)m[i * 4 + 2] * (double)base.pos[2] + (double)m[i * 4 + 3]);
            }
            if (nm_known_finite || skin_matrix4_is_finite(nm)) {
                for (int i = 0; i < 3; i++) {
                    nrm[i] += w * ((double)nm[i * 4 + 0] * (double)base.normal[0] +
                                   (double)nm[i * 4 + 1] * (double)base.normal[1] +
                                   (double)nm[i * 4 + 2] * (double)base.normal[2]);
                    tan[i] += w * ((double)nm[i * 4 + 0] * (double)base.tangent[0] +
                                   (double)nm[i * 4 + 1] * (double)base.tangent[1] +
                                   (double)nm[i * 4 + 2] * (double)base.tangent[2]);
                }
                normal_influences++;
                tangent_influences++;
            }
            total_w += w;
        }

        if (extra) {
            for (int b = 0; b < 4; b++) {
                double w = skin_weight_or_skip(extra[v].weights[b]);
                int idx;
                if (w <= 0.0)
                    continue;
                idx = (int)extra[v].indices[b];
                if (idx >= bone_count)
                    continue;
                {
                    const float *m = &palette[(size_t)idx * 16u];
                    float nm_local[16];
                    const float *nm;
                    if (!skin_matrix4_is_finite(m))
                        continue;
                    if (normal_palette && idx < normal_bone_count) {
                        nm = &normal_palette[(size_t)idx * 16u];
                    } else {
                        vgfx3d_compute_normal_matrix4(m, nm_local);
                        nm = nm_local;
                    }
                    for (int i = 0; i < 3; i++) {
                        pos[i] += w * ((double)m[i * 4 + 0] * (double)base.pos[0] +
                                       (double)m[i * 4 + 1] * (double)base.pos[1] +
                                       (double)m[i * 4 + 2] * (double)base.pos[2] +
                                       (double)m[i * 4 + 3]);
                    }
                    if (skin_matrix4_is_finite(nm)) {
                        for (int i = 0; i < 3; i++) {
                            nrm[i] += w * ((double)nm[i * 4 + 0] * (double)base.normal[0] +
                                           (double)nm[i * 4 + 1] * (double)base.normal[1] +
                                           (double)nm[i * 4 + 2] * (double)base.normal[2]);
                            tan[i] += w * ((double)nm[i * 4 + 0] * (double)base.tangent[0] +
                                           (double)nm[i * 4 + 1] * (double)base.tangent[1] +
                                           (double)nm[i * 4 + 2] * (double)base.tangent[2]);
                        }
                        normal_influences++;
                        tangent_influences++;
                    }
                    total_w += w;
                }
            }
        }

        /* Copy all attributes, then overwrite position and normal */
        dst[v] = base;

        if (total_w > 1e-6) {
            double inv_w = 1.0 / total_w;
            pos[0] *= inv_w;
            pos[1] *= inv_w;
            pos[2] *= inv_w;
            if (skin_vec3_is_usable(pos)) {
                float px = (float)pos[0];
                float py = (float)pos[1];
                float pz = (float)pos[2];
                if (isfinite(px) && isfinite(py) && isfinite(pz)) {
                    dst[v].pos[0] = px;
                    dst[v].pos[1] = py;
                    dst[v].pos[2] = pz;
                }
            }
            /* Normalize the skinned normal. The weighted sum's magnitude is
             * irrelevant once renormalized, so we skip the inv_w scale here. If
             * opposing bone influences cancel to a near-zero vector there is no
             * meaningful direction to keep — leave the source normal (already
             * copied into dst above) rather than writing a degenerate zero
             * normal that would black out shading on that vertex. */
            if (normal_influences > 0)
                skin_store_normalized_vec3(nrm, dst[v].normal);
            if (tangent_influences > 0) {
                double tangent_vec[3] = {tan[0], tan[1], tan[2]};
                double normal_vec[3] = {
                    (double)dst[v].normal[0], (double)dst[v].normal[1], (double)dst[v].normal[2]};
                double n_len2 = normal_vec[0] * normal_vec[0] + normal_vec[1] * normal_vec[1] +
                                normal_vec[2] * normal_vec[2];
                if (isfinite(n_len2) && n_len2 > 1e-16) {
                    double dot = tangent_vec[0] * normal_vec[0] + tangent_vec[1] * normal_vec[1] +
                                 tangent_vec[2] * normal_vec[2];
                    if (isfinite(dot)) {
                        double along_normal = dot / n_len2;
                        tangent_vec[0] -= along_normal * normal_vec[0];
                        tangent_vec[1] -= along_normal * normal_vec[1];
                        tangent_vec[2] -= along_normal * normal_vec[2];
                    }
                }
                skin_store_normalized_vec3(tangent_vec, dst[v].tangent);
            }
        }
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
