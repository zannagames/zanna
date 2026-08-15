//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_mesh3d.c
// Purpose: Zanna.Graphics3D.Mesh3D — dynamic vertex/index mesh storage
//   with programmatic construction and procedural generators.
//
// Key invariants:
//   - Vertices stored as vgfx3d_vertex_t (92 bytes, float internally)
//   - Indices are uint32_t (max 16M vertices per mesh)
//   - CCW winding is front-facing
//   - Deferred draws retain immutable geometry revisions; mutation forks rather
//     than editing bytes already referenced by a queued command.
//
// Ownership/Lifetime:
//   - rt_mesh3d is GC-managed; its finalizer frees authored arrays, retained
//     revision ownership, and physics/scene raycast acceleration structures.
//
// Links: rt_canvas3d.h, rt_canvas3d_internal.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements mutable Mesh3D storage, validation, cloning, and geometry transforms.
/// @details Meshes own their authored vertex/index arrays and optional double-precision position
///          sidecars, while retaining animation objects referenced by the mesh. Mutation advances
///          revision epochs so deferred draws and derived caches never reuse stale geometry.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_alloc_size.h"
#include "rt_asset_error.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_file_stdio.h"
#include "rt_g3d_ref_slots.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_untrusted_count.h"
#include "rt_vec3.h"
#include "vgfx3d_backend_utils.h"

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
#include "rt_trap.h"
extern const char *rt_string_cstr(rt_string s);

#include <limits.h>
#define MESH_INIT_VERTS 64
#define MESH_INIT_IDXS 128
#define MESH_MAX_SPHERE_SEGMENTS 512
#define MESH_MAX_CYLINDER_SEGMENTS 8192
#define MESH3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define MESH3D_OBJ_MAX_LINE_BYTES (1024u * 1024u)
#define MESH3D_OBJ_MAX_FACE_VERTS 4096
/// @brief Above this many vertices, obj_triangulate_face emits a triangle fan instead of
/// O(n^2..n^3) ear-clipping (mirrors the FBX loader's FBX_MAX_EARCLIP_POLY_VERTS fallback).
/// A fan is exact for convex faces; concave faces this large are vanishingly rare in OBJ and
/// degrade gracefully rather than stalling the loader on a pathological mega-face.
#define MESH3D_OBJ_MAX_EARCLIP_VERTS 1024
#define MESH3D_STL_ASCII_MAX_LINE_BYTES (1024u * 1024u)
#define MESH3D_PROCEDURAL_MAX_BYTES (128ull * 1024ull * 1024ull)
#define MESH3D_CLEAR_RETAIN_VERTEX_CAP 65536u
#define MESH3D_CLEAR_RETAIN_INDEX_CAP 196608u
#define MESH3D_POSITION64_PRECISION_RISK_THRESHOLD 65536.0
#define MESH3D_POSITION64_MATERIAL_DELTA 1e-3
#define MESH3D_NORMAL_STACK_VERTS 256u
#if defined(__clang__) || defined(__GNUC__)
#define MESH3D_UNUSED_PRIVATE __attribute__((unused))
#else
#define MESH3D_UNUSED_PRIVATE
#endif

static volatile uint64_t g_mesh3d_geometry_epoch = 1u;

/// @brief Publish a process-wide notification that mesh geometry changed.
/// @details The release-ordered increment invalidates consumers that cache scene-wide geometry
///          decisions independently of an individual mesh revision.
void rt_mesh3d_note_global_geometry_change(void) {
    (void)rt_atomic_fetch_add_u64(&g_mesh3d_geometry_epoch, UINT64_C(1), __ATOMIC_RELEASE);
}

/// @brief Read the process-wide mesh geometry revision epoch.
/// @return Acquire-loaded epoch value suitable for detecting geometry changes since a prior read.
uint64_t rt_mesh3d_global_geometry_epoch(void) {
    return rt_atomic_load_u64(&g_mesh3d_geometry_epoch, __ATOMIC_ACQUIRE);
}

/// @brief Validate @p obj as a Mesh3D handle and return its typed pointer (NULL on mismatch).
/// @param obj Candidate runtime object handle.
/// @return Borrowed Mesh3D pointer, or NULL when @p obj is NULL or belongs to another class.
static rt_mesh3d *mesh3d_checked(void *obj) {
    return rt_obj_is_instance(obj, RT_G3D_MESH3D_CLASS_ID, sizeof(rt_mesh3d)) ? (rt_mesh3d *)obj
                                                                              : NULL;
}

/// @brief Increment a diagnostic counter without allowing unsigned wraparound.
/// @param counter Counter to advance; NULL is accepted as a no-op.
static void mesh3d_increment_counter(uint64_t *counter) {
    if (counter && *counter != UINT64_MAX)
        (*counter)++;
}

#include "rt_mesh3d_retained.inc"

/// @brief Release oversized empty mesh buffers after Clear while keeping small reuse capacity.
/// @details Dynamic meshes often clear and rebuild every frame; retaining modest buffers avoids
///          churn. Very large one-off meshes, however, should not pin vertex/index/positions64
///          memory after they become empty, so this helper frees capacities above conservative
///          thresholds.
/// @param m Empty mesh whose reusable allocations may be reduced; NULL and nonempty meshes are
///          ignored.
static void mesh3d_shrink_empty_storage_if_oversized(rt_mesh3d *m) {
    if (!m || m->vertex_count != 0 || m->index_count != 0)
        return;
    if (m->vertex_capacity > MESH3D_CLEAR_RETAIN_VERTEX_CAP) {
        free(m->vertices);
        free(m->positions64);
        m->vertices = NULL;
        m->positions64 = NULL;
        m->vertex_capacity = 0;
    }
    if (m->index_capacity > MESH3D_CLEAR_RETAIN_INDEX_CAP) {
        free(m->indices);
        m->indices = NULL;
        m->index_capacity = 0;
    }
    free(m->normal_accum_scratch);
    m->normal_accum_scratch = NULL;
    m->normal_accum_scratch_values = 0u;
}

/// @brief Return zero-filled scratch for per-vertex normal accumulation.
///
/// @details Small meshes use caller-provided stack storage. Larger meshes reuse a buffer owned by
///          the mesh so repeated `RecalcNormals` calls and importer normal repairs do not allocate
///          and free a large accumulator each time.
/// @param m Mesh that owns reusable heap scratch for large accumulators.
/// @param stack_accum Caller-provided scratch used when @p accum_values fits.
/// @param stack_values Number of double elements available in @p stack_accum.
/// @param accum_values Number of zeroed double elements requested.
/// @return Pointer to @p accum_values zeroed doubles, or NULL when allocation fails.
static double *mesh3d_prepare_normal_accumulator(rt_mesh3d *m,
                                                 double *stack_accum,
                                                 size_t stack_values,
                                                 size_t accum_values) {
    double *next;
    if (accum_values <= stack_values) {
        memset(stack_accum, 0, accum_values * sizeof(double));
        return stack_accum;
    }
    if (!m)
        return NULL;
    if (m->normal_accum_scratch_values < accum_values) {
        if (accum_values > SIZE_MAX / sizeof(double))
            return NULL;
        next = (double *)realloc(m->normal_accum_scratch, accum_values * sizeof(double));
        if (!next)
            return NULL;
        m->normal_accum_scratch = next;
        m->normal_accum_scratch_values = accum_values;
    }
    memset(m->normal_accum_scratch, 0, accum_values * sizeof(double));
    return m->normal_accum_scratch;
}

/// @brief Bump the mesh's geometry-revision counter to invalidate cached GPU/derived data
///   (wrapping past UINT32_MAX to 1, since 0 marks "never set"); optionally also drops the
///   cached tangents so they are recomputed.
/// @param m Mesh whose geometry-dependent revisions and caches are invalidated; NULL is ignored.
/// @param invalidate_tangents Nonzero to mark cached tangents stale in addition to geometry.
static void mesh3d_bump_vertex_revision(rt_mesh3d *m, int invalidate_tangents) {
    if (!m)
        return;
    if (!m->transient_geometry_facade)
        rt_mesh3d_invalidate_retained_geometry(m);
    m->resident = 1;
    if (m->geometry_revision == UINT32_MAX)
        m->geometry_revision = 1;
    else
        m->geometry_revision++;
    if (invalidate_tangents) {
        m->tangents_ready = 0;
        m->tangent_revision = 0;
    }
    m->validated_index_revision = 0;
    m->validated_index_count = 0;
    m->positions64_rebase_revision = 0;
    m->positions64_rebase_needed = 0;
    m->morph_bound_deltas_source = NULL;
    m->morph_bound_revision = 0;
    m->morph_bound_vertex_count = 0;
    m->morph_bound_shape_count = 0;
    m->morph_bound_pad = 0.0;
    m->morph_bound_valid = 0;
    if (!m->transient_geometry_facade)
        rt_mesh3d_note_global_geometry_change();
}

/// @brief Estimate vertex/index payload bytes, saturating to INT64_MAX for ABI stability.
/// @param m Mesh whose safe live element counts are measured.
/// @return Combined live vertex and index bytes, zero for NULL, or `INT64_MAX` on overflow.
static int64_t mesh3d_estimate_payload_bytes(const rt_mesh3d *m) {
    uint64_t vertex_bytes;
    uint64_t index_bytes;
    uint64_t total;
    if (!m)
        return 0;
    vertex_bytes = (uint64_t)rt_mesh3d_safe_vertex_count(m) * (uint64_t)sizeof(vgfx3d_vertex_t);
    index_bytes = (uint64_t)rt_mesh3d_safe_index_count(m) * (uint64_t)sizeof(uint32_t);
    total = vertex_bytes + index_bytes;
    if (total < vertex_bytes)
        return INT64_MAX;
    return total > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)total;
}

/// @brief Repair private index-buffer corruption before cloning a Mesh3D.
/// @details Programmatic AddTriangle already enforces in-range indices, but tests and internal
///          repair paths intentionally tolerate corrupt private counts. When a count repair exposes
///          unused capacity slots as real indices, clamp any out-of-range slot to zero so the clone
///          cannot later feed undefined vertex fetches to GPU backends. Existing public
///          count-repair semantics are preserved; draw submission still rejects incomplete
///          triangle-list tails.
/// @param m Mesh whose live index range is repaired; NULL is rejected.
/// @param op Reserved caller context for diagnostics.
/// @return One after successful validation or repair, or zero for a NULL mesh.
static int mesh3d_sanitize_triangle_indices(rt_mesh3d *m, const char *op) {
    uint32_t vertex_count;
    uint32_t index_count;
    int repaired = 0;
    if (!m)
        return 0;
    vertex_count = rt_mesh3d_safe_vertex_count(m);
    index_count = rt_mesh3d_safe_index_count(m);
    if (index_count == 0)
        return 1;
    if (vertex_count == 0) {
        m->index_count = 0;
        mesh3d_bump_vertex_revision(m, 1);
        return 1;
    }
    for (uint32_t i = 0; i < index_count; i++) {
        if (m->indices[i] >= vertex_count) {
            m->indices[i] = 0;
            repaired = 1;
        }
    }
    if (repaired)
        mesh3d_bump_vertex_revision(m, 1);
    (void)op;
    return 1;
}

/// @brief Check a planned vertex/index allocation against a predictable procedural budget.
/// @param vertex_count Planned vertex count.
/// @param index_count Planned scalar index count.
/// @param trap_name Optional trap message used when arithmetic or budget validation fails.
/// @return One when counts and their worst-case storage fit the limit; otherwise zero after
/// trapping.
static int mesh3d_check_planned_payload(uint64_t vertex_count,
                                        uint64_t index_count,
                                        const char *trap_name) {
    uint64_t vertex_bytes;
    uint64_t position_sidecar_bytes;
    uint64_t index_bytes;
    uint64_t total;
    if (vertex_count > UINT32_MAX || index_count > UINT32_MAX)
        goto too_large;
    if (vertex_count > UINT64_MAX / (uint64_t)sizeof(vgfx3d_vertex_t))
        goto too_large;
    vertex_bytes = vertex_count * (uint64_t)sizeof(vgfx3d_vertex_t);
    if (vertex_count > UINT64_MAX / (uint64_t)(3u * sizeof(double)))
        goto too_large;
    position_sidecar_bytes = vertex_count * (uint64_t)(3u * sizeof(double));
    if (index_count > UINT64_MAX / (uint64_t)sizeof(uint32_t))
        goto too_large;
    index_bytes = index_count * (uint64_t)sizeof(uint32_t);
    if (UINT64_MAX - vertex_bytes < index_bytes)
        goto too_large;
    total = vertex_bytes + index_bytes;
    if (UINT64_MAX - total < position_sidecar_bytes)
        goto too_large;
    total += position_sidecar_bytes;
    if (total <= MESH3D_PROCEDURAL_MAX_BYTES)
        return 1;
too_large:
    rt_trap(trap_name ? trap_name : "Mesh3D: procedural mesh exceeds safe memory budget");
    return 0;
}

/// @brief Return 10 raised to an integer exponent without calling libm `pow`.
/// @details OBJ and ASCII STL loaders parse many coordinates in tight loops. Exponentiation by
/// squaring keeps decimal exponent scaling logarithmic in the exponent magnitude and avoids the
/// comparatively expensive general-purpose `pow(10.0, n)` path for every numeric token. Overflow or
/// underflow is left to IEEE-754 multiplication and rejected by the caller's `isfinite` check.
/// @param exponent Signed base-10 exponent to evaluate.
/// @return A double scale factor approximately equal to ten raised to @p exponent.
static double mesh_pow10_i64(int64_t exponent) {
    uint64_t remaining;
    double base = exponent < 0 ? 0.1 : 10.0;
    double scale = 1.0;
    if (exponent < 0)
        remaining = (uint64_t)(-exponent);
    else
        remaining = (uint64_t)exponent;
    while (remaining != 0u) {
        if ((remaining & 1u) != 0u)
            scale *= base;
        remaining >>= 1u;
        if (remaining != 0u)
            base *= base;
    }
    return scale;
}

/// @brief Parse an OBJ/STL ASCII float with '.' decimal semantics, independent of locale.
/// @details Accepts an optional sign, fractional component, and bounded decimal exponent. The
///          parser stops at the first nonnumeric byte and rejects non-finite results.
/// @param p First byte of the numeric token.
/// @param limit Optional one-past-end bound; NULL permits parsing through the terminating byte.
/// @param out_end Receives the first unconsumed byte on success.
/// @param out Receives the finite parsed value on success.
/// @return One on success, or zero for invalid pointers, malformed input, excessive exponent, or
///         non-finite output.
static int mesh_parse_ascii_double_span(const char *p,
                                        const char *limit,
                                        const char **out_end,
                                        double *out) {
    const char *s = p;
    double value = 0.0;
    int sign = 1;
    int digits = 0;
    int frac_digits = 0;
    int exp_sign = 1;
    int exp_value = 0;
    int exp_digits = 0;
    double result;
    if (!s || !out_end || !out)
        return 0;
    if ((!limit || s < limit) && (*s == '+' || *s == '-')) {
        if (*s == '-')
            sign = -1;
        s++;
    }
    while ((!limit || s < limit) && *s >= '0' && *s <= '9') {
        value = value * 10.0 + (double)(*s - '0');
        digits++;
        s++;
    }
    if ((!limit || s < limit) && *s == '.') {
        s++;
        while ((!limit || s < limit) && *s >= '0' && *s <= '9') {
            value = value * 10.0 + (double)(*s - '0');
            digits++;
            frac_digits++;
            s++;
        }
    }
    if (digits == 0)
        return 0;
    if ((!limit || s < limit) && (*s == 'e' || *s == 'E')) {
        const char *exp_start = s;
        s++;
        if ((!limit || s < limit) && (*s == '+' || *s == '-')) {
            if (*s == '-')
                exp_sign = -1;
            s++;
        }
        while ((!limit || s < limit) && *s >= '0' && *s <= '9') {
            if (exp_digits >= 4)
                return 0;
            exp_value = exp_value * 10 + (*s - '0');
            if (exp_value > 512 + frac_digits)
                return 0;
            exp_digits++;
            s++;
        }
        if (exp_digits == 0)
            s = exp_start;
    }
    result = (double)sign * value *
             mesh_pow10_i64((int64_t)exp_sign * (int64_t)exp_value - (int64_t)frac_digits);
    if (!isfinite(result))
        return 0;
    *out_end = s;
    *out = result;
    return 1;
}

/// @brief Return 1 if any vertex carries a non-zero bone weight, else 0.
/// @details Used by Mesh3D.Clone to decide whether to propagate `bone_count` —
///          meshes without skinning data clone with `bone_count = 0`.
/// @param mesh Mesh whose live vertices are inspected.
/// @return One when any finite weight has meaningful magnitude; otherwise zero.
static int mesh3d_has_bone_weights(const rt_mesh3d *mesh) {
    uint32_t vertex_count = rt_mesh3d_safe_vertex_count(mesh);
    if (!mesh || !mesh->vertices)
        return 0;
    for (uint32_t i = 0; i < vertex_count; i++) {
        const vgfx3d_vertex_t *v = &mesh->vertices[i];
        for (int j = 0; j < 4; j++) {
            if (isfinite(v->bone_weights[j]) && fabsf(v->bone_weights[j]) > 1e-8f)
                return 1;
        }
    }
    return 0;
}

/// @brief Validate @p obj is a live Mat4 heap object.
/// @param obj Candidate runtime matrix handle.
/// @return Borrowed Mat4 implementation pointer, or NULL for invalid input.
static mat4_impl *mesh3d_mat4_checked(void *obj) {
    if (!obj || !rt_obj_is_instance(obj, RT_MAT4_CLASS_ID, sizeof(mat4_impl)))
        return NULL;
    return (mat4_impl *)obj;
}

/// @brief Test whether @p value is finite and within ±FLT_MAX for safe `double → float` narrowing.
/// @param value Double-precision value to test.
/// @return Nonzero when conversion to float stays finite and in range; otherwise zero.
static int mesh_value_fits_float(double value) {
    return isfinite(value) && value >= -MESH3D_FLOAT_ABS_MAX && value <= MESH3D_FLOAT_ABS_MAX;
}

/// @brief Return whether an authored double coordinate should promote the mesh to positions64.
/// @details Ordinary local meshes do not need the double sidecar: the backend and culling paths
///          already operate on float vertices. The sidecar is reserved for coordinates that are
///          large enough to need camera-relative vertex rebasing, or whose float narrowing loses a
///          material amount of precision. This mirrors the Canvas3D rebase predicate so AddVertex
///          meshes pay the memory cost only when the extra precision can affect rendering.
/// @param value Authored double-precision position component.
/// @return Nonzero when the coordinate cannot safely rely on float-only storage.
static int mesh_position_component_needs_positions64(double value) {
    float narrowed;
    if (!mesh_value_fits_float(value))
        return 1;
    if (fabs(value) > MESH3D_POSITION64_PRECISION_RISK_THRESHOLD)
        return 1;
    narrowed = (float)value;
    return fabs(value - (double)narrowed) > MESH3D_POSITION64_MATERIAL_DELTA;
}

/// @brief Return whether a vertex position should allocate the optional double-position sidecar.
/// @param x Authored X coordinate.
/// @param y Authored Y coordinate.
/// @param z Authored Z coordinate.
/// @return Nonzero when any coordinate requires double-precision sidecar storage.
static int mesh_position_needs_positions64(double x, double y, double z) {
    return mesh_position_component_needs_positions64(x) ||
           mesh_position_component_needs_positions64(y) ||
           mesh_position_component_needs_positions64(z);
}

/// @brief Sanitize copied vertex data that may have come from imported or direct internal storage.
/// @details Non-finite position, normal, tangent, UV, and weight lanes are replaced with safe
///          defaults; tangent handedness is canonicalized to negative or positive one.
/// @param v Vertex record to sanitize in place; NULL is ignored.
static void mesh3d_sanitize_vertex_copy(vgfx3d_vertex_t *v) {
    if (!v)
        return;
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(v->pos[i]))
            v->pos[i] = 0.0f;
        if (!isfinite(v->normal[i]))
            v->normal[i] = 0.0f;
        if (!isfinite(v->tangent[i]))
            v->tangent[i] = 0.0f;
    }
    for (int i = 0; i < 2; ++i) {
        if (!isfinite(v->uv[i]))
            v->uv[i] = 0.0f;
        if (!isfinite(v->uv1[i]))
            v->uv1[i] = v->uv[i];
    }
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(v->bone_weights[i]))
            v->bone_weights[i] = 0.0f;
    }
    if (!isfinite(v->tangent[3]) || v->tangent[3] == 0.0f)
        v->tangent[3] = 1.0f;
    else
        v->tangent[3] = v->tangent[3] < 0.0f ? -1.0f : 1.0f;
}

/// @brief Ensure the optional double-position sidecar exists and mirrors existing vertices.
/// @details Programmatic AddVertex meshes preserve authoring precision here so the
///          camera-relative upload path can subtract the frame origin before float
///          narrowing. Meshes loaded by importers that directly assign float vertex
///          buffers simply leave this NULL and use the existing float positions.
/// @param m Mesh that should own the sidecar.
/// @param label Operation label embedded in allocation trap messages.
/// @return One when the sidecar already exists or was populated successfully; otherwise zero.
static int mesh3d_ensure_positions64(rt_mesh3d *m, const char *label) {
    char msg[160];
    if (!m)
        return 0;
    if (m->positions64)
        return 1;
    if (!rt_alloc_count_ok(m->vertex_capacity, 3u * sizeof(double))) {
        snprintf(msg, sizeof(msg), "%s: position sidecar allocation overflow", label);
        rt_trap(msg);
        return 0;
    }
    m->positions64 = (double *)calloc((size_t)m->vertex_capacity * 3u, sizeof(double));
    if (!m->positions64) {
        snprintf(msg, sizeof(msg), "%s: memory allocation failed", label);
        rt_trap(msg);
        return 0;
    }
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        m->positions64[(size_t)i * 3u + 0] = (double)m->vertices[i].pos[0];
        m->positions64[(size_t)i * 3u + 1] = (double)m->vertices[i].pos[1];
        m->positions64[(size_t)i * 3u + 2] = (double)m->vertices[i].pos[2];
    }
    return 1;
}

/// @brief True if all 16 lanes of a 4x4 float matrix are finite (no NaN/Inf).
/// @param m Pointer to sixteen matrix elements.
/// @return One when every element is finite; zero for NULL or any non-finite lane.
static int mesh_matrix4f_is_finite(const float *m) {
    if (!m)
        return 0;
    for (int i = 0; i < 16; i++) {
        if (!isfinite(m[i]))
            return 0;
    }
    return 1;
}

/// @brief Squared face-normal length for three float positions.
/// @param a First three-component position.
/// @param b Second three-component position.
/// @param c Third three-component position.
/// @return Squared magnitude of the two-edge cross product, proportional to area squared.
static double mesh_triangle_area_sq_f32(const float *a, const float *b, const float *c) {
    double abx = (double)b[0] - (double)a[0];
    double aby = (double)b[1] - (double)a[1];
    double abz = (double)b[2] - (double)a[2];
    double acx = (double)c[0] - (double)a[0];
    double acy = (double)c[1] - (double)a[1];
    double acz = (double)c[2] - (double)a[2];
    double nx = aby * acz - abz * acy;
    double ny = abz * acx - abx * acz;
    double nz = abx * acy - aby * acx;
    return nx * nx + ny * ny + nz * nz;
}

/// @brief Squared area of triangle (a, b, c) in double precision (half the cross-product
/// magnitude²).
/// @details Used as a degeneracy test that avoids a sqrt; near-zero means a sliver/collinear
/// triangle.
/// @param a First three-component position.
/// @param b Second three-component position.
/// @param c Third three-component position.
/// @return Squared magnitude of the two-edge cross product, proportional to area squared.
static double mesh_triangle_area_sq_f64(const double *a, const double *b, const double *c) {
    double abx = b[0] - a[0];
    double aby = b[1] - a[1];
    double abz = b[2] - a[2];
    double acx = c[0] - a[0];
    double acy = c[1] - a[1];
    double acz = c[2] - a[2];
    double nx = aby * acz - abz * acy;
    double ny = abz * acx - abx * acz;
    double nz = abx * acy - aby * acx;
    return nx * nx + ny * ny + nz * nz;
}

/// @brief Return one mesh position as double precision, preferring authoritative positions64.
/// @param mesh Mesh containing the position.
/// @param index Vertex index to read.
/// @param out Three-element destination; invalid sources produce the zero vector.
static void mesh_position_f64_at(const rt_mesh3d *mesh, uint32_t index, double out[3]) {
    if (!out)
        return;
    if (mesh && mesh->positions64 && index < mesh->vertex_count) {
        const double *p = &mesh->positions64[(size_t)index * 3u];
        out[0] = isfinite(p[0]) ? p[0] : 0.0;
        out[1] = isfinite(p[1]) ? p[1] : 0.0;
        out[2] = isfinite(p[2]) ? p[2] : 0.0;
        return;
    }
    if (mesh && mesh->vertices && index < mesh->vertex_count) {
        out[0] = (double)mesh->vertices[index].pos[0];
        out[1] = (double)mesh->vertices[index].pos[1];
        out[2] = (double)mesh->vertices[index].pos[2];
        return;
    }
    out[0] = out[1] = out[2] = 0.0;
}

/// @brief Scale-aware squared-area threshold for triangle degeneracy tests.
/// @details Area squared has fourth-power length units, so the epsilon scales with the square of
///   the largest
///   edge length squared. This avoids rejecting valid large-world triangles because of a tiny
///   absolute threshold, while still filtering sub-ULP slivers near the origin.
/// @param a First three-component position.
/// @param b Second three-component position.
/// @param c Third three-component position.
/// @return Finite squared-area threshold derived from the longest edge.
static double mesh_triangle_area_epsilon_sq_f64(const double *a, const double *b, const double *c) {
    double abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    double acx = c[0] - a[0], acy = c[1] - a[1], acz = c[2] - a[2];
    double bcx = c[0] - b[0], bcy = c[1] - b[1], bcz = c[2] - b[2];
    double ab2 = abx * abx + aby * aby + abz * abz;
    double ac2 = acx * acx + acy * acy + acz * acz;
    double bc2 = bcx * bcx + bcy * bcy + bcz * bcz;
    double max_edge_sq = fmax(ab2, fmax(ac2, bc2));
    if (!isfinite(max_edge_sq) || max_edge_sq <= 0.0)
        return 1e-24;
    return fmax(1e-24, max_edge_sq * max_edge_sq * 1e-12);
}

/// @brief Return non-zero when three float positions define a usable triangle.
/// @param a First three-component position.
/// @param b Second three-component position.
/// @param c Third three-component position.
/// @return Nonzero when inputs form a finite triangle above the scale-aware degeneracy threshold.
static int mesh_positions_form_triangle(const float *a, const float *b, const float *c) {
    double da[3], db[3], dc[3];
    double area_sq;
    if (!a || !b || !c)
        return 0;
    da[0] = (double)a[0];
    da[1] = (double)a[1];
    da[2] = (double)a[2];
    db[0] = (double)b[0];
    db[1] = (double)b[1];
    db[2] = (double)b[2];
    dc[0] = (double)c[0];
    dc[1] = (double)c[1];
    dc[2] = (double)c[2];
    area_sq = mesh_triangle_area_sq_f32(a, b, c);
    return isfinite(area_sq) && area_sq > mesh_triangle_area_epsilon_sq_f64(da, db, dc);
}

/// @brief Return non-zero when three mesh indices define a usable, non-degenerate face.
/// @param mesh Mesh containing the indexed positions.
/// @param i0 First vertex index.
/// @param i1 Second vertex index.
/// @param i2 Third vertex index.
/// @return Nonzero when indices are distinct, in range, and form a finite nondegenerate triangle.
static int mesh_indices_form_triangle(const rt_mesh3d *mesh,
                                      uint32_t i0,
                                      uint32_t i1,
                                      uint32_t i2) {
    if (!mesh || !mesh->vertices || i0 == i1 || i1 == i2 || i0 == i2 || i0 >= mesh->vertex_count ||
        i1 >= mesh->vertex_count || i2 >= mesh->vertex_count)
        return 0;
    if (mesh->positions64) {
        const double *a = &mesh->positions64[(size_t)i0 * 3u];
        const double *b = &mesh->positions64[(size_t)i1 * 3u];
        const double *c = &mesh->positions64[(size_t)i2 * 3u];
        double area_sq = mesh_triangle_area_sq_f64(a, b, c);
        return isfinite(area_sq) && area_sq > mesh_triangle_area_epsilon_sq_f64(a, b, c);
    }
    return mesh_positions_form_triangle(
        mesh->vertices[i0].pos, mesh->vertices[i1].pos, mesh->vertices[i2].pos);
}

/// @brief Public non-trapping triangle-validity probe (see rt_canvas3d.h).
/// @details Shares mesh_indices_form_triangle so asset loaders filter with exactly
///   the criterion AddTriangle enforces — a fixed loader-side epsilon that diverges
///   from the mesh's scale-relative epsilon lets slivers through and traps imports.
/// @param obj Mesh3D handle to inspect.
/// @param v0 First candidate vertex index.
/// @param v1 Second candidate vertex index.
/// @param v2 Third candidate vertex index.
/// @return Nonzero when all indices form a valid triangle; otherwise zero without trapping.
int rt_mesh3d_triangle_indices_valid(void *obj, int64_t v0, int64_t v1, int64_t v2) {
    const rt_mesh3d *m = (const rt_mesh3d *)obj;
    if (!m || v0 < 0 || v1 < 0 || v2 < 0)
        return 0;
    if ((uint64_t)v0 >= m->vertex_count || (uint64_t)v1 >= m->vertex_count ||
        (uint64_t)v2 >= m->vertex_count)
        return 0;
    return mesh_indices_form_triangle(m, (uint32_t)v0, (uint32_t)v1, (uint32_t)v2);
}

/// @brief Guard for procedural-generator dimensions — trap if `value` is NaN/inf or ≤ 0.
/// @details The generators (`new_box`, `new_sphere`, `new_plane`, `new_cylinder`) all
///   require strictly positive, finite extents; any other value would produce degenerate
///   geometry or runaway loops. The trap message includes `label` so the caller site
///   (e.g. "Mesh3D.NewBox: sx") is identifiable in the user-facing error.
/// @param value Dimension to validate.
/// @param label Parameter label included in the trap message.
/// @return One for a positive finite float-representable value; otherwise zero after trapping.
static int mesh_validate_positive_finite(double value, const char *label) {
    char msg[128];
    if (mesh_value_fits_float(value) && value > 0.0)
        return 1;
    snprintf(
        msg, sizeof(msg), "%s must be finite, fit float range, and be greater than zero", label);
    rt_trap(msg);
    return 0;
}

/// @brief Release and discard a partially-built mesh if its build_failed flag is set.
/// @details Called at the end of every procedural generator (new_box, new_sphere,
///   new_plane, new_cylinder) and after the OBJ/STL loaders.  If any intermediate
///   add_vertex / add_triangle call trapped and set build_failed, this function
///   releases the GC reference so the partially-constructed mesh is freed, and
///   returns NULL to the caller.  NULL input and a clean mesh are both forwarded
///   unchanged, so the generator can use this as a tail-call return.
/// @param mesh Nullable newly built Mesh3D handle.
/// @return @p mesh when clean, or NULL after releasing a failed partial build.
static void *mesh_return_null_if_build_failed(void *mesh) {
    if (!mesh || !((rt_mesh3d *)mesh)->build_failed)
        return mesh;
    if (rt_obj_release_check0(mesh))
        rt_obj_free(mesh);
    return NULL;
}

/// @brief Latch the `build_failed` bit on a mesh so subsequent builder calls bail early.
/// @details Once any add-vertex / add-triangle step traps, the mesh is in an indeterminate
///   state. Rather than mutating in place and risking inconsistent index/vertex counts,
///   we sticky-set this flag and let the OBJ/STL loader detect it and free the partially
///   built mesh.
/// @param mesh Mesh whose build is marked failed; NULL is ignored.
static void mesh_mark_build_failed(rt_mesh3d *mesh) {
    if (mesh)
        mesh->build_failed = 1;
}

/// @brief Allocate grown vertex storage without mutating the mesh until all allocations succeed.
/// @details `realloc` can commit one buffer before a later sidecar allocation fails. This helper
///          allocates replacement vertex storage, copies the live vertices, and optionally
///          allocates a replacement double-position sidecar. The caller commits both pointers
///          together.
/// @param m Mesh whose live vertex data should be copied.
/// @param vertex_capacity Replacement vertex capacity in elements.
/// @param label Caller label used in trap messages.
/// @param out_vertices Receives the replacement vertex buffer.
/// @param out_positions64 Receives the replacement sidecar, or NULL when no sidecar is active.
/// @return Non-zero when every requested buffer was allocated and copied.
static int mesh3d_alloc_grown_vertex_storage(rt_mesh3d *m,
                                             uint32_t vertex_capacity,
                                             const char *label,
                                             vgfx3d_vertex_t **out_vertices,
                                             double **out_positions64) {
    char msg[160];
    vgfx3d_vertex_t *vertices;
    double *positions64 = NULL;
    size_t vertex_bytes;
    if (!m || !out_vertices || !out_positions64)
        return 0;
    *out_vertices = NULL;
    *out_positions64 = NULL;
    if (!rt_alloc_count_ok(vertex_capacity, sizeof(vgfx3d_vertex_t))) {
        snprintf(msg, sizeof(msg), "%s: vertex allocation overflow", label);
        rt_trap(msg);
        return 0;
    }
    vertex_bytes = (size_t)vertex_capacity * sizeof(vgfx3d_vertex_t);
    vertices = (vgfx3d_vertex_t *)malloc(vertex_bytes);
    if (!vertices) {
        snprintf(msg, sizeof(msg), "%s: memory allocation failed", label);
        rt_trap(msg);
        return 0;
    }
    if (m->vertices && m->vertex_count > 0)
        memcpy(vertices, m->vertices, (size_t)m->vertex_count * sizeof(vgfx3d_vertex_t));
    if (m->positions64) {
        if (!rt_alloc_count_ok(vertex_capacity, 3u * sizeof(double))) {
            snprintf(msg, sizeof(msg), "%s: position sidecar allocation overflow", label);
            rt_trap(msg);
            free(vertices);
            return 0;
        }
        positions64 = (double *)malloc((size_t)vertex_capacity * 3u * sizeof(double));
        if (!positions64) {
            snprintf(msg, sizeof(msg), "%s: memory allocation failed", label);
            rt_trap(msg);
            free(vertices);
            return 0;
        }
        if (m->vertex_count > 0)
            memcpy(positions64, m->positions64, (size_t)m->vertex_count * 3u * sizeof(double));
    }
    *out_vertices = vertices;
    *out_positions64 = positions64;
    return 1;
}

/// @brief Ensure vertex and index buffers can hold at least the requested capacities.
/// @details Vertex storage and its optional position sidecar are committed atomically. Index
///          growth uses realloc only after overflow checks; existing capacities are preserved.
/// @param m Mesh whose backing arrays may grow.
/// @param vertex_capacity Minimum vertex-element capacity.
/// @param index_capacity Minimum scalar-index capacity.
/// @param label Operation label embedded in allocation trap messages.
/// @return One when requested capacities are available; otherwise zero.
static int mesh3d_reserve_storage(rt_mesh3d *m,
                                  uint32_t vertex_capacity,
                                  uint32_t index_capacity,
                                  const char *label) {
    char msg[160];
    if (!m)
        return 0;
    if (vertex_capacity > m->vertex_capacity) {
        vgfx3d_vertex_t *nv;
        double *np = NULL;
        if (!mesh3d_alloc_grown_vertex_storage(m, vertex_capacity, label, &nv, &np))
            return 0;
        free(m->vertices);
        free(m->positions64);
        m->vertices = nv;
        m->positions64 = np;
        m->vertex_capacity = vertex_capacity;
    }
    if (index_capacity > m->index_capacity) {
        uint32_t *ni;
        if (!rt_alloc_count_ok(index_capacity, sizeof(uint32_t))) {
            snprintf(msg, sizeof(msg), "%s: index allocation overflow", label);
            rt_trap(msg);
            return 0;
        }
        ni = (uint32_t *)realloc(m->indices, (size_t)index_capacity * sizeof(uint32_t));
        if (!ni) {
            snprintf(msg, sizeof(msg), "%s: memory allocation failed", label);
            rt_trap(msg);
            return 0;
        }
        m->indices = ni;
        m->index_capacity = index_capacity;
    }
    return 1;
}

/// @brief Build a stable tangent orthogonal to `normal` when UVs are degenerate.
/// @param normal Optional source normal; invalid or near-zero values use positive Z.
/// @param tangent Four-element destination receiving a unit tangent and positive handedness.
static void mesh_default_tangent_from_normal(const float *normal, float *tangent) {
    float n[3] = {0.0f, 0.0f, 1.0f};
    if (normal && isfinite(normal[0]) && isfinite(normal[1]) && isfinite(normal[2])) {
        float len = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (isfinite(len) && len > 1e-8f) {
            n[0] = normal[0] / len;
            n[1] = normal[1] / len;
            n[2] = normal[2] / len;
        }
    }

    float axis[3] = {fabsf(n[0]) < 0.9f ? 1.0f : 0.0f, fabsf(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
    float dot = axis[0] * n[0] + axis[1] * n[1] + axis[2] * n[2];
    tangent[0] = axis[0] - n[0] * dot;
    tangent[1] = axis[1] - n[1] * dot;
    tangent[2] = axis[2] - n[2] * dot;
    {
        float len =
            sqrtf(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
        if (!isfinite(len) || len <= 1e-8f) {
            tangent[0] = 1.0f;
            tangent[1] = 0.0f;
            tangent[2] = 0.0f;
        } else {
            tangent[0] /= len;
            tangent[1] /= len;
            tangent[2] /= len;
        }
    }
    tangent[3] = 1.0f;
}

/// @brief Decrement-and-free on a GC-tracked reference slot; no-op if the slot is NULL.
/// @details Paired with `mesh_assign_ref` to implement `retain-then-release` ordering on
///   the `morph_targets_ref` field. The slot is cleared to NULL after release so a
///   subsequent assignment can't accidentally double-release.
/// @param slot Address of an owned GC reference to release and clear.
static void mesh_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release an owned Skeleton3D slot only when it still stores a Skeleton3D.
/// @details Wrong-class private state is treated as borrowed corruption and cleared without
///          releasing; matching Skeleton3D slots are owned and released normally.
/// @param slot Address of the retained skeleton slot.
static void mesh_release_skeleton_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_SKELETON3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    mesh_release_ref(slot);
}

/// @brief Release an owned MorphTarget3D slot only when it still stores a MorphTarget3D.
/// @details Wrong-class private state is treated as borrowed corruption and cleared without
///          releasing; matching MorphTarget3D slots are owned and released normally.
/// @param slot Address of the retained morph-target slot.
static void mesh_release_morph_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MORPHTARGET3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    mesh_release_ref(slot);
}

/// @brief Safely assign a Skeleton3D ref into a retained mesh slot.
/// @param slot Address of the retained skeleton slot; NULL is ignored.
/// @param value Nullable replacement Skeleton3D, retained before the current value is released.
static void mesh_assign_skeleton_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    mesh_release_skeleton_slot(slot);
    *slot = value;
}

/// @brief Safely assign a MorphTarget3D ref into a retained mesh slot.
/// @param slot Address of the retained morph-target slot; NULL is ignored.
/// @param value Nullable replacement MorphTarget3D, retained before the current value is released.
static void mesh_assign_morph_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    rt_obj_retain_maybe(value);
    mesh_release_morph_slot(slot);
    *slot = value;
}

/// @brief Release and clear corrupted animation refs.
/// @param m Mesh whose retained skeleton and morph-target classes are validated; NULL is ignored.
static void mesh_repair_animation_refs(rt_mesh3d *m) {
    if (!m)
        return;
    if (m->skeleton_ref && !rt_g3d_has_class(m->skeleton_ref, RT_G3D_SKELETON3D_CLASS_ID))
        mesh_release_skeleton_slot(&m->skeleton_ref);
    if (m->morph_targets_ref &&
        !rt_g3d_has_class(m->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID))
        mesh_release_morph_slot(&m->morph_targets_ref);
}

/// @brief Drop transient animation payload pointers that are owned by the current draw frame.
/// @details `bone_palette`, `morph_deltas`, and related pointers are borrowed snapshots produced by
///          animation controllers during draw submission. Mesh3D owns only the retained skeleton
///          and morph-target handles, so Clear must null these transient pointers rather than
///          attempting to free storage that belongs to another subsystem.
/// @param m Mesh whose frame-borrowed animation state is detached; NULL is ignored.
static void mesh3d_clear_transient_animation_payloads(rt_mesh3d *m) {
    if (!m)
        return;
    m->bone_palette = NULL;
    m->prev_bone_palette = NULL;
    m->bone_count = 0;
    m->bone_map = NULL;
    m->extra_influences = NULL;
    m->morph_deltas = NULL;
    m->morph_normal_deltas = NULL;
    m->morph_weights = NULL;
    m->prev_morph_weights = NULL;
    m->morph_shape_count = 0;
    m->morph_bound_deltas_source = NULL;
    m->morph_bound_revision = 0;
    m->morph_bound_vertex_count = 0;
    m->morph_bound_shape_count = 0;
    m->morph_bound_pad = 0.0;
    m->morph_bound_valid = 0;
}

/// @brief GC finalizer for Mesh3D — releases the owned vertex and index buffers.
/// @details Also releases retained geometry revisions, acceleration structures, scratch buffers,
///          skinning maps, and owned animation references.
/// @param obj Mesh3D instance being finalized.
static void rt_mesh3d_finalize(void *obj) {
    rt_mesh3d *m = (rt_mesh3d *)obj;
    rt_mesh3d_invalidate_retained_geometry(m);
    free(m->vertices);
    m->vertices = NULL;
    free(m->positions64);
    m->positions64 = NULL;
    free(m->indices);
    m->indices = NULL;
    free(m->submesh_ranges);
    m->submesh_ranges = NULL;
    m->submesh_range_count = 0;
    m->submesh_range_capacity = 0;
    free(m->normal_accum_scratch);
    m->normal_accum_scratch = NULL;
    m->normal_accum_scratch_values = 0u;
    free(m->physics_bvh_nodes);
    m->physics_bvh_nodes = NULL;
    free(m->physics_bvh_tri_indices);
    m->physics_bvh_tri_indices = NULL;
    free(m->raycast_bvh_nodes);
    m->raycast_bvh_nodes = NULL;
    free(m->raycast_bvh_tri_indices);
    m->raycast_bvh_tri_indices = NULL;
    free(m->bone_map);
    m->bone_map = NULL;
    free(m->extra_influences);
    m->extra_influences = NULL;
    mesh_release_skeleton_slot(&m->skeleton_ref);
    mesh_release_morph_slot(&m->morph_targets_ref);
}

/// @brief Allocate and initialize a Mesh3D object with optional default growable buffers.
/// @details Shared constructor used by both the public `Mesh3D.New` path and exact-size importer
///          paths. When @p allocate_default_storage is non-zero, the mesh receives the small
///          default vertex/index arrays used by programmatic append workflows; otherwise the mesh
///          starts with zero capacity and callers must assign or reserve storage before adding
///          geometry.
/// @param allocate_default_storage Nonzero to allocate append-friendly initial arrays.
/// @return New initialized Mesh3D, or NULL after trapping on object or storage allocation failure.
static rt_mesh3d *mesh3d_new_initialized(int allocate_default_storage) {
    rt_mesh3d *m = (rt_mesh3d *)rt_obj_new_i64(RT_G3D_MESH3D_CLASS_ID, (int64_t)sizeof(rt_mesh3d));
    if (!m) {
        rt_trap("Mesh3D.New: memory allocation failed");
        return NULL;
    }
    m->vptr = NULL;
    m->identity_serial = rt_g3d_next_identity_serial();
    m->vertices = allocate_default_storage
                      ? (vgfx3d_vertex_t *)calloc(MESH_INIT_VERTS, sizeof(vgfx3d_vertex_t))
                      : NULL;
    m->positions64 = NULL;
    m->vertex_count = 0;
    m->vertex_capacity = allocate_default_storage ? MESH_INIT_VERTS : 0;
    m->indices =
        allocate_default_storage ? (uint32_t *)calloc(MESH_INIT_IDXS, sizeof(uint32_t)) : NULL;
    m->index_count = 0;
    m->index_capacity = allocate_default_storage ? MESH_INIT_IDXS : 0;
    m->submesh_ranges = NULL;
    m->submesh_range_count = 0;
    m->submesh_range_capacity = 0;
    m->simplify_requested_triangles = 0;
    m->simplify_achieved_triangles = 0;
    m->simplify_status = RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN;
    m->normal_accum_scratch = NULL;
    m->normal_accum_scratch_values = 0u;
    m->bone_palette = NULL;
    m->prev_bone_palette = NULL;
    m->bone_count = 0;
    m->morph_deltas = NULL;
    m->morph_normal_deltas = NULL;
    m->morph_weights = NULL;
    m->prev_morph_weights = NULL;
    m->morph_shape_count = 0;
    m->morph_bound_deltas_source = NULL;
    m->morph_bound_revision = 0;
    m->morph_bound_vertex_count = 0;
    m->morph_bound_shape_count = 0;
    m->morph_bound_pad = 0.0;
    m->morph_bound_valid = 0;
    m->skeleton_ref = NULL;
    m->morph_targets_ref = NULL;
    m->build_failed = 0;
    m->geometry_revision = 1;
    m->tangent_revision = 0;
    m->tangents_ready = 0;
    m->positions64_rebase_revision = 0;
    m->positions64_rebase_needed = 0;
    m->resident = 1;
    m->compact_streams = 0;
    m->geometry_batch_depth = 0;
    m->geometry_batch_dirty = 0;
    m->transient_geometry_facade = 0;
    m->physics_bvh_nodes = NULL;
    m->physics_bvh_tri_indices = NULL;
    m->physics_bvh_revision = 0;
    m->physics_bvh_node_count = 0;
    m->physics_bvh_tri_count = 0;
    m->raycast_bvh_nodes = NULL;
    m->raycast_bvh_tri_indices = NULL;
    m->raycast_bvh_revision = 0;
    m->raycast_bvh_node_count = 0;
    m->raycast_bvh_tri_count = 0;
    m->raycast_bvh_rebuild_count = 0;
    m->raycast_last_triangle_probe_count = 0;
    m->retained_geometry = NULL;
    m->retained_geometry_build_count = 0;
    m->retained_geometry_cache_hit_count = 0;
    m->retained_tangent_build_count = 0;
    m->retained_tangent_cache_hit_count = 0;
    m->retained_tangent_cache_key = 0;
    rt_mesh3d_reset_bounds(m);
    if (allocate_default_storage && (!m->vertices || !m->indices)) {
        free(m->vertices);
        free(m->positions64);
        free(m->indices);
        m->vertices = NULL;
        m->positions64 = NULL;
        m->indices = NULL;
        if (rt_obj_release_check0(m))
            rt_obj_free(m);
        rt_trap("Mesh3D.New: memory allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(m, rt_mesh3d_finalize);
    return m;
}

/// @brief Create a new empty 3D mesh for programmatic construction.
/// @details Allocates vertex and index arrays with initial capacity. Vertices are
///          stored as vgfx3d_vertex_t (92 bytes each, float internally) and indices
///          as uint32_t. The mesh supports up to 16M vertices. Geometry is built
///          by calling add_vertex/add_triangle, or by using the procedural generators
///          (new_box, new_sphere, new_plane, new_cylinder). GC finalizer frees arrays.
/// @return Opaque mesh handle, or NULL on allocation failure.
void *rt_mesh3d_new(void) {
    return mesh_return_null_if_build_failed(mesh3d_new_initialized(1));
}

/// @brief Create a Mesh3D with initialized metadata but no default vertex/index arrays.
/// @details Exact-size importers use this to avoid the allocation/free pair that would otherwise
///          happen when replacing `Mesh3D.New`'s small default buffers with decoded asset payloads.
///          The object is otherwise a normal Mesh3D and is finalized by `rt_mesh3d_finalize`.
/// @return New zero-capacity Mesh3D handle, or NULL on allocation failure.
void *rt_mesh3d_new_empty_storage(void) {
    return mesh_return_null_if_build_failed(mesh3d_new_initialized(0));
}

/// @brief Remove all vertices and indices from the mesh, resetting to empty.
/// @details Clears submesh metadata, animation bindings, acceleration structures, build status,
///          and bounds. Small buffers remain available for reuse; oversized empty allocations are
///          released before the geometry revision is advanced.
/// @param obj Mesh3D receiver; invalid handles are ignored.
void rt_mesh3d_clear(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m)
        return;
    m->vertex_count = 0;
    m->index_count = 0;
    free(m->submesh_ranges);
    m->submesh_ranges = NULL;
    m->submesh_range_count = 0;
    m->submesh_range_capacity = 0;
    m->simplify_requested_triangles = 0;
    m->simplify_achieved_triangles = 0;
    m->simplify_status = RT_MESH3D_SIMPLIFY_STATUS_NOT_RUN;
    mesh3d_clear_transient_animation_payloads(m);
    m->build_failed = 0;
    mesh_release_skeleton_slot(&m->skeleton_ref);
    mesh_release_morph_slot(&m->morph_targets_ref);
    free(m->physics_bvh_nodes);
    m->physics_bvh_nodes = NULL;
    free(m->physics_bvh_tri_indices);
    m->physics_bvh_tri_indices = NULL;
    m->physics_bvh_revision = 0;
    m->physics_bvh_node_count = 0;
    m->physics_bvh_tri_count = 0;
    free(m->raycast_bvh_nodes);
    m->raycast_bvh_nodes = NULL;
    free(m->raycast_bvh_tri_indices);
    m->raycast_bvh_tri_indices = NULL;
    m->raycast_bvh_revision = 0;
    m->raycast_bvh_node_count = 0;
    m->raycast_bvh_tri_count = 0;
    mesh3d_shrink_empty_storage_if_oversized(m);
    rt_mesh3d_touch_geometry(m);
    rt_mesh3d_reset_bounds(m);
}

/// @brief Reserve backing storage for at least vertex_count vertices and triangle_count triangles.
/// @details Counts must be non-negative and fit the uint32 index representation. Existing live
///          geometry is preserved and capacities never shrink.
/// @param obj Mesh3D receiver; invalid or failed-build meshes are ignored.
/// @param vertex_count Minimum vertex capacity.
/// @param triangle_count Minimum triangle capacity, converted to three scalar indices per face.
void rt_mesh3d_reserve(void *obj, int64_t vertex_count, int64_t triangle_count) {
    rt_mesh3d *m = mesh3d_checked(obj);
    uint32_t vertex_capacity;
    uint32_t index_capacity;
    if (!m)
        return;
    if (m->build_failed)
        return;
    if (vertex_count < 0 || triangle_count < 0) {
        rt_trap("Mesh3D.Reserve: capacities must be non-negative");
        return;
    }
    if ((uint64_t)vertex_count > UINT32_MAX || (uint64_t)triangle_count > (UINT32_MAX / 3u)) {
        rt_trap("Mesh3D.Reserve: capacity overflow");
        return;
    }
    vertex_capacity = (uint32_t)vertex_count;
    index_capacity = (uint32_t)((uint64_t)triangle_count * 3u);
    (void)mesh3d_reserve_storage(m, vertex_capacity, index_capacity, "Mesh3D.Reserve");
}

/// @brief Add a vertex with position, normal, and UV texture coordinates.
/// @details The vertex array grows geometrically when full. Normals should be
///          normalized (or recalculated later with recalc_normals). UV coords
///          use the standard [0,1] range. The vertex is stored as float internally
///          (vgfx3d_vertex_t), converted from the double parameters.
/// @param obj Mesh3D receiver; invalid or failed-build meshes are ignored.
/// @param x Vertex X coordinate.
/// @param y Vertex Y coordinate.
/// @param z Vertex Z coordinate.
/// @param nx Vertex-normal X component.
/// @param ny Vertex-normal Y component.
/// @param nz Vertex-normal Z component.
/// @param u Primary texture U coordinate, also copied to the secondary UV set.
/// @param v Primary texture V coordinate, also copied to the secondary UV set.
void rt_mesh3d_add_vertex(
    void *obj, double x, double y, double z, double nx, double ny, double nz, double u, double v) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m)
        return;
    if (m->build_failed)
        return;
    if (!mesh_value_fits_float(x) || !mesh_value_fits_float(y) || !mesh_value_fits_float(z) ||
        !mesh_value_fits_float(nx) || !mesh_value_fits_float(ny) || !mesh_value_fits_float(nz) ||
        !mesh_value_fits_float(u) || !mesh_value_fits_float(v)) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddVertex: vertex attributes must be finite and fit float range");
        return;
    }

    if (m->vertex_count == UINT32_MAX) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddVertex: vertex capacity overflow");
        return;
    }

    if (m->vertex_count >= m->vertex_capacity) {
        uint32_t new_cap;
        if (m->vertex_capacity > UINT32_MAX / 2u) {
            mesh_mark_build_failed(m);
            rt_trap("Mesh3D.AddVertex: vertex capacity overflow");
            return;
        }
        new_cap = m->vertex_capacity * 2u;
        if (new_cap <= m->vertex_count)
            new_cap = m->vertex_count + 1u;
        if (!mesh3d_reserve_storage(m, new_cap, m->index_capacity, "Mesh3D.AddVertex")) {
            mesh_mark_build_failed(m);
            return;
        }
    }
    if ((m->positions64 || mesh_position_needs_positions64(x, y, z)) &&
        !mesh3d_ensure_positions64(m, "Mesh3D.AddVertex")) {
        mesh_mark_build_failed(m);
        return;
    }

    uint32_t vertex_index = m->vertex_count++;
    vgfx3d_vertex_t *vt = &m->vertices[vertex_index];
    memset(vt, 0, sizeof(vgfx3d_vertex_t));
    if (m->positions64) {
        m->positions64[(size_t)vertex_index * 3u + 0] = x;
        m->positions64[(size_t)vertex_index * 3u + 1] = y;
        m->positions64[(size_t)vertex_index * 3u + 2] = z;
    }
    vt->pos[0] = (float)x;
    vt->pos[1] = (float)y;
    vt->pos[2] = (float)z;
    vt->normal[0] = (float)nx;
    vt->normal[1] = (float)ny;
    vt->normal[2] = (float)nz;
    vt->uv[0] = (float)u;
    vt->uv[1] = (float)v;
    vt->uv1[0] = (float)u;
    vt->uv1[1] = (float)v;
    vt->color[0] = 1.0f;
    vt->color[1] = 1.0f;
    vt->color[2] = 1.0f;
    vt->color[3] = 1.0f;
    vt->tangent[3] = 1.0f;
    rt_mesh3d_touch_geometry(m);
}

/// @brief Add a triangle defined by three vertex indices (CCW winding = front-facing).
/// @details Negative, out-of-range, repeated, or geometrically degenerate indices trap and latch
///          the mesh's failed-build state. Successful insertion grows index storage as needed and
///          advances the geometry revision.
/// @param obj Mesh3D receiver; invalid or failed-build meshes are ignored.
/// @param v0 First vertex index.
/// @param v1 Second vertex index.
/// @param v2 Third vertex index.
void rt_mesh3d_add_triangle(void *obj, int64_t v0, int64_t v1, int64_t v2) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m)
        return;
    if (m->build_failed)
        return;

    if (v0 < 0 || v1 < 0 || v2 < 0) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddTriangle: vertex index must be non-negative");
        return;
    }
    if ((uint64_t)v0 >= m->vertex_count || (uint64_t)v1 >= m->vertex_count ||
        (uint64_t)v2 >= m->vertex_count) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddTriangle: vertex index out of range");
        return;
    }
    if (v0 == v1 || v1 == v2 || v0 == v2) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddTriangle: degenerate triangle");
        return;
    }
    if (!mesh_indices_form_triangle(m, (uint32_t)v0, (uint32_t)v1, (uint32_t)v2)) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddTriangle: degenerate triangle");
        return;
    }

    if (m->index_count > UINT32_MAX - 3u) {
        mesh_mark_build_failed(m);
        rt_trap("Mesh3D.AddTriangle: index capacity overflow");
        return;
    }

    uint32_t needed = m->index_count + 3u;
    if (needed > m->index_capacity) {
        uint32_t new_cap;
        if (m->index_capacity > UINT32_MAX / 2u) {
            mesh_mark_build_failed(m);
            rt_trap("Mesh3D.AddTriangle: index capacity overflow");
            return;
        }
        new_cap = m->index_capacity * 2u;
        if (new_cap < needed)
            new_cap = needed;
        if (!rt_alloc_count_ok(new_cap, sizeof(uint32_t))) {
            mesh_mark_build_failed(m);
            rt_trap("Mesh3D.AddTriangle: index allocation overflow");
            return;
        }
        uint32_t *ni = (uint32_t *)realloc(m->indices, (size_t)new_cap * sizeof(uint32_t));
        if (!ni) {
            mesh_mark_build_failed(m);
            rt_trap("Mesh3D.AddTriangle: memory allocation failed");
            return;
        }
        m->indices = ni;
        m->index_capacity = new_cap;
    }

    m->indices[m->index_count++] = (uint32_t)v0;
    m->indices[m->index_count++] = (uint32_t)v1;
    m->indices[m->index_count++] = (uint32_t)v2;
    rt_mesh3d_touch_geometry(m);
}

/// @brief Get the number of vertices in the mesh.
/// @param obj Mesh3D receiver.
/// @return Repaired live vertex count, or zero for an invalid mesh.
int64_t rt_mesh3d_get_vertex_count(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    return m ? (int64_t)rt_mesh3d_safe_vertex_count(m) : 0;
}

/// @brief Return one mesh-local vertex position as a managed Vec3.
/// @details Reads the authoritative double-precision position sidecar when present, otherwise
///          widens the drawable float position. This read-only accessor never exposes mutable
///          vertex storage.
/// @param obj Mesh3D receiver.
/// @param index Zero-based vertex index.
/// @return Fresh GC-managed Vec3 for a valid live vertex, or NULL for an invalid mesh or index.
void *rt_mesh3d_get_vertex_position(void *obj, int64_t index) {
    rt_mesh3d *m = mesh3d_checked(obj);
    uint32_t vertex_count = m ? rt_mesh3d_safe_vertex_count(m) : 0;
    double position[3];
    if (!m || index < 0 || (uint64_t)index >= (uint64_t)vertex_count)
        return NULL;
    mesh_position_f64_at(m, (uint32_t)index, position);
    return rt_vec3_new(position[0], position[1], position[2]);
}

/// @brief Get the number of triangles in the mesh (index_count / 3).
/// @param obj Mesh3D receiver.
/// @return Number of complete triangles in the safe live index range, or zero for invalid input.
int64_t rt_mesh3d_get_triangle_count(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    return m ? (int64_t)(rt_mesh3d_safe_index_count(m) / 3u) : 0;
}

/// @brief Read one vertex's position/normal/uv (internal: HLOD proxy bake readback).
/// @param obj Mesh3D receiver.
/// @param index Zero-based vertex index.
/// @param out_pos Optional three-double position destination.
/// @param out_normal Optional three-double normal destination.
/// @param out_uv Optional two-double primary UV destination.
/// @return 1 on success, 0 for an invalid mesh or out-of-range index.
int8_t rt_mesh3d_get_vertex_raw(
    void *obj, int64_t index, double out_pos[3], double out_normal[3], double out_uv[2]) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m || index < 0 || index >= (int64_t)rt_mesh3d_safe_vertex_count(m))
        return 0;
    const vgfx3d_vertex_t *v = &m->vertices[index];
    if (out_pos) {
        out_pos[0] = v->pos[0];
        out_pos[1] = v->pos[1];
        out_pos[2] = v->pos[2];
    }
    if (out_normal) {
        out_normal[0] = v->normal[0];
        out_normal[1] = v->normal[1];
        out_normal[2] = v->normal[2];
    }
    if (out_uv) {
        out_uv[0] = v->uv[0];
        out_uv[1] = v->uv[1];
    }
    return 1;
}

/// @brief Read one triangle's vertex indices (internal: HLOD proxy bake readback).
/// @param obj Mesh3D receiver.
/// @param triangle Zero-based triangle index.
/// @param out_indices Optional three-element destination for scalar vertex indices.
/// @return 1 on success, 0 for an invalid mesh or out-of-range triangle.
int8_t rt_mesh3d_get_triangle_raw(void *obj, int64_t triangle, int64_t out_indices[3]) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m || triangle < 0 || triangle >= (int64_t)(rt_mesh3d_safe_index_count(m) / 3u))
        return 0;
    if (out_indices) {
        out_indices[0] = m->indices[triangle * 3 + 0];
        out_indices[1] = m->indices[triangle * 3 + 1];
        out_indices[2] = m->indices[triangle * 3 + 2];
    }
    return 1;
}

/// @brief Return whether this mesh's vertex/index payload is resident.
/// @param obj Mesh3D receiver.
/// @return One when draw/upload residency is enabled, or zero for invalid or nonresident meshes.
int8_t rt_mesh3d_get_resident(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    return (m && m->resident) ? 1 : 0;
}

/// @brief Mark a mesh payload eligible or ineligible for draw-time residency.
/// @details Meshes can be procedural or imported without a reload handle, so toggling
///          residency never frees the CPU vertex/index payload. The flag is instead
///          a conservative draw/cache hint: nonresident meshes are skipped by draw
///          submission, and setting resident true makes the preserved payload drawable
///          again without data loss.
/// @param obj Mesh3D receiver; invalid handles are ignored.
/// @param resident Zero to disable draw residency, or nonzero to enable it.
void rt_mesh3d_set_resident(void *obj, int8_t resident) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m)
        return;
    m->resident = resident ? 1 : 0;
}

/// @brief Opt a mesh into (or out of) the compact GPU vertex-stream encoding (R20).
/// @details Affects only static-geometry-cache uploads on GPU backends: cached vertices
///          upload in the packed 48-byte layout instead of the full 92-byte record. The
///          CPU payload, software rendering, skinning, morphing, and physics are
///          unchanged. Toggling bumps the geometry revision so backend caches re-upload
///          in the newly selected encoding.
/// @param obj Mesh3D receiver; invalid handles are ignored.
/// @param enabled Zero for the full stream, or nonzero for compact cached GPU streams.
void rt_mesh3d_set_compact_streams(void *obj, int8_t enabled) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m)
        return;
    enabled = enabled ? 1 : 0;
    if (m->compact_streams == enabled)
        return;
    m->compact_streams = enabled;
    rt_mesh3d_touch_geometry(m);
}

/// @brief Whether the mesh opted into the compact GPU vertex-stream encoding.
/// @param obj Mesh3D receiver.
/// @return One when compact streams are enabled, or zero for invalid or full-stream meshes.
int8_t rt_mesh3d_get_compact_streams(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    return (m && m->compact_streams) ? 1 : 0;
}

/// @brief Active resident vertex/index byte estimate.
/// @details `Mesh3D.Resident` controls draw/upload eligibility, not payload
///          ownership: the geometry remains retained so a later `SetResident(true)`
///          can restore drawing without a source reload. Nonresident meshes report
///          zero resident bytes while still keeping CPU payloads intact.
/// @param obj Mesh3D receiver.
/// @return Live vertex/index byte estimate while resident, zero while nonresident or invalid, or
///         `INT64_MAX` when the estimate saturates.
int64_t rt_mesh3d_get_resident_bytes(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m || !m->resident)
        return 0;
    return mesh3d_estimate_payload_bytes(m);
}

/// @brief Retained vertex/index byte estimate, independent of draw residency.
/// @details `Mesh3D.Resident = false` prevents draw/upload submission but does not release
///          procedural or imported CPU payloads. This accessor reports the retained payload
///          size so streaming systems can distinguish hidden resident bytes from actual RAM use.
/// @param obj Mesh3D receiver.
/// @return Live retained vertex/index byte estimate, zero for invalid input, or `INT64_MAX` on
///         saturation.
int64_t rt_mesh3d_get_retained_bytes(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    if (!m)
        return 0;
    return mesh3d_estimate_payload_bytes(m);
}

/// @brief `Mesh3D.ReleaseCpuScratch()` — free auxiliary CPU-side geometry a
///   finished static mesh no longer needs.
/// @details Releases the double-precision position stream (24 bytes/vertex,
///   kept by AddVertex-built meshes for raycast/rebase precision) and the
///   normal-recalculation accumulator. The float vertex/index arrays are
///   RETAINED: backends re-upload from them after GPU cache eviction, the
///   software backend and CPU skinning read them every frame, and raycasts
///   fall back to them (float precision) once positions64 is gone. Returns the
///   number of bytes released. Safe to call at any time; a later AddVertex
///   simply re-promotes precision from that point on.
/// @param obj Mesh3D receiver.
/// @return Number of position-sidecar and normal-scratch bytes released, or zero for invalid input.
int64_t rt_mesh3d_release_cpu_scratch(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    int64_t released = 0;
    if (!m)
        return 0;
    if (m->positions64) {
        released += (int64_t)((size_t)m->vertex_capacity * 3u * sizeof(double));
        free(m->positions64);
        m->positions64 = NULL;
        m->positions64_rebase_revision = 0;
        m->positions64_rebase_needed = 0;
    }
    if (m->normal_accum_scratch) {
        released += (int64_t)(m->normal_accum_scratch_values * sizeof(double));
        free(m->normal_accum_scratch);
        m->normal_accum_scratch = NULL;
        m->normal_accum_scratch_values = 0;
    }
    return released;
}

/// @brief Recalculate smooth vertex normals by averaging face normals per-vertex.
/// @details Uses double accumulation and scale-aware degeneracy rejection. Meshes without complete
///          faces receive the positive-Y fallback normal; completion invalidates tangents and all
///          geometry-derived caches.
/// @param obj Mesh3D receiver; invalid handles are ignored.
void rt_mesh3d_recalc_normals(void *obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    uint32_t vertex_count;
    uint32_t index_count;
    double stack_accum[MESH3D_NORMAL_STACK_VERTS * 3u];
    double *accum;
    size_t accum_values;
    if (!m)
        return;
    rt_mesh3d_repair_geometry_counts(m);
    vertex_count = rt_mesh3d_safe_vertex_count(m);
    index_count = rt_mesh3d_safe_index_count(m);
    if (index_count < 3u) {
        for (uint32_t i = 0; i < vertex_count; i++) {
            m->vertices[i].normal[0] = 0.0f;
            m->vertices[i].normal[1] = 1.0f;
            m->vertices[i].normal[2] = 0.0f;
        }
        mesh3d_bump_vertex_revision(m, 1);
        return;
    }
    if (!rt_alloc_count_ok(vertex_count, 3u * sizeof(double))) {
        rt_trap("Mesh3D.RecalcNormals: normal accumulator allocation overflow");
        return;
    }
    accum_values = (size_t)vertex_count * 3u;
    accum = mesh3d_prepare_normal_accumulator(
        m, stack_accum, (size_t)MESH3D_NORMAL_STACK_VERTS * 3u, accum_values);
    if (!accum) {
        rt_trap("Mesh3D.RecalcNormals: memory allocation failed");
        return;
    }

    /* Zero all normals */
    for (uint32_t i = 0; i < vertex_count; i++) {
        m->vertices[i].normal[0] = 0.0f;
        m->vertices[i].normal[1] = 0.0f;
        m->vertices[i].normal[2] = 0.0f;
    }

    /* Accumulate face normals */
    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint32_t i0 = m->indices[i], i1 = m->indices[i + 1], i2 = m->indices[i + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
            continue;

        double p0[3], p1[3], p2[3];
        mesh_position_f64_at(m, i0, p0);
        mesh_position_f64_at(m, i1, p1);
        mesh_position_f64_at(m, i2, p2);

        double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};

        double nx = e1[1] * e2[2] - e1[2] * e2[1];
        double ny = e1[2] * e2[0] - e1[0] * e2[2];
        double nz = e1[0] * e2[1] - e1[1] * e2[0];
        double len_sq = nx * nx + ny * ny + nz * nz;
        if (!isfinite(len_sq) || len_sq <= mesh_triangle_area_epsilon_sq_f64(p0, p1, p2))
            continue;
        accum[(size_t)i0 * 3u + 0] += nx;
        accum[(size_t)i0 * 3u + 1] += ny;
        accum[(size_t)i0 * 3u + 2] += nz;
        accum[(size_t)i1 * 3u + 0] += nx;
        accum[(size_t)i1 * 3u + 1] += ny;
        accum[(size_t)i1 * 3u + 2] += nz;
        accum[(size_t)i2 * 3u + 0] += nx;
        accum[(size_t)i2 * 3u + 1] += ny;
        accum[(size_t)i2 * 3u + 2] += nz;
    }

    /* Normalize */
    for (uint32_t i = 0; i < vertex_count; i++) {
        float *n = m->vertices[i].normal;
        double nx = accum[(size_t)i * 3u + 0];
        double ny = accum[(size_t)i * 3u + 1];
        double nz = accum[(size_t)i * 3u + 2];
        double len = sqrt(nx * nx + ny * ny + nz * nz);
        if (isfinite(len) && len > 1e-12 && mesh_value_fits_float(nx / len) &&
            mesh_value_fits_float(ny / len) && mesh_value_fits_float(nz / len)) {
            n[0] = (float)(nx / len);
            n[1] = (float)(ny / len);
            n[2] = (float)(nz / len);
        } else {
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
        }
    }
    mesh3d_bump_vertex_revision(m, 1);
}

/// @brief Rasterize the UV footprint of triangles whose object-space Y sits inside
///   [y_min, y_max] into a Pixels mask (opaque white texels; everything else untouched).
/// @details Body-zone texture masking: a clothing region of a character atlas is
///   identified by the bind-pose HEIGHT of the geometry that samples it, letting
///   per-region recolors (jersey vs pants) work on atlases whose colors alone
///   cannot be separated. UVs map to texels exactly like the samplers do
///   (u * width, v * height, repeat-wrapped); a triangle is included only when
///   all three vertices sit inside the Y band. The mask must be an ordinary
///   Pixels object; it is mutated in place.
/// @param obj Mesh3D receiver; invalid handles are ignored.
/// @param mask_pixels Pixels handle receiving the coverage (any size).
/// @param y_min Inclusive lower object-space Y bound.
/// @param y_max Inclusive upper object-space Y bound.
void rt_mesh3d_rasterize_uv_mask_y(void *obj, void *mask_pixels, double y_min, double y_max) {
    rt_mesh3d *m = mesh3d_checked(obj);
    int64_t w;
    int64_t h;
    uint32_t vertex_count;
    uint32_t index_count;
    if (!m || !mask_pixels)
        return;
    w = rt_pixels_width(mask_pixels);
    h = rt_pixels_height(mask_pixels);
    if (w <= 0 || h <= 0)
        return;
    rt_mesh3d_repair_geometry_counts(m);
    vertex_count = rt_mesh3d_safe_vertex_count(m);
    index_count = rt_mesh3d_safe_index_count(m);
    for (uint32_t i = 0; i + 2 < index_count; i += 3) {
        uint32_t i0 = m->indices[i], i1 = m->indices[i + 1], i2 = m->indices[i + 2];
        double ax, ay, bx, by, cx, cy;
        double min_x, max_x, min_y, max_y;
        double area;
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count)
            continue;
        if (m->vertices[i0].pos[1] < y_min || m->vertices[i0].pos[1] > y_max ||
            m->vertices[i1].pos[1] < y_min || m->vertices[i1].pos[1] > y_max ||
            m->vertices[i2].pos[1] < y_min || m->vertices[i2].pos[1] > y_max)
            continue;
        /* Repeat-wrap each UV into [0,1) (the atlases in scope never span the
         * seam), then scale onto the texel grid. */
        ax = ((double)m->vertices[i0].uv[0] - floor((double)m->vertices[i0].uv[0])) * (double)w;
        ay = ((double)m->vertices[i0].uv[1] - floor((double)m->vertices[i0].uv[1])) * (double)h;
        bx = ((double)m->vertices[i1].uv[0] - floor((double)m->vertices[i1].uv[0])) * (double)w;
        by = ((double)m->vertices[i1].uv[1] - floor((double)m->vertices[i1].uv[1])) * (double)h;
        cx = ((double)m->vertices[i2].uv[0] - floor((double)m->vertices[i2].uv[0])) * (double)w;
        cy = ((double)m->vertices[i2].uv[1] - floor((double)m->vertices[i2].uv[1])) * (double)h;
        if (!isfinite(ax) || !isfinite(ay) || !isfinite(bx) || !isfinite(by) || !isfinite(cx) ||
            !isfinite(cy))
            continue;
        min_x = ax;
        max_x = ax;
        min_y = ay;
        max_y = ay;
        if (bx < min_x)
            min_x = bx;
        if (bx > max_x)
            max_x = bx;
        if (cx < min_x)
            min_x = cx;
        if (cx > max_x)
            max_x = cx;
        if (by < min_y)
            min_y = by;
        if (by > max_y)
            max_y = by;
        if (cy < min_y)
            min_y = cy;
        if (cy > max_y)
            max_y = cy;
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (!isfinite(area) || (area > -1e-9 && area < 1e-9))
            continue;
        {
            int64_t x_lo = (int64_t)floor(min_x);
            int64_t x_hi = (int64_t)ceil(max_x);
            int64_t y_lo = (int64_t)floor(min_y);
            int64_t y_hi = (int64_t)ceil(max_y);
            if (x_lo < 0)
                x_lo = 0;
            if (y_lo < 0)
                y_lo = 0;
            if (x_hi > w - 1)
                x_hi = w - 1;
            if (y_hi > h - 1)
                y_hi = h - 1;
            for (int64_t py = y_lo; py <= y_hi; py++) {
                for (int64_t px = x_lo; px <= x_hi; px++) {
                    double sx = (double)px + 0.5;
                    double sy = (double)py + 0.5;
                    double w0 = (bx - ax) * (sy - ay) - (by - ay) * (sx - ax);
                    double w1 = (cx - bx) * (sy - by) - (cy - by) * (sx - bx);
                    double w2 = (ax - cx) * (sy - cy) - (ay - cy) * (sx - cx);
                    if (area > 0.0) {
                        if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0)
                            continue;
                    } else {
                        if (w0 > 0.0 || w1 > 0.0 || w2 > 0.0)
                            continue;
                    }
                    rt_pixels_set_rgba(mask_pixels, px, py, (int64_t)0xFFFFFFFFu);
                }
            }
        }
    }
}

/// @brief Compute per-vertex normals from the mesh's triangle faces when the source provided
///   none, accumulating area-weighted face normals and normalizing. Traps on accumulator
///   allocation overflow.
/// @param m Imported mesh whose zero-length normals are filled in place.
/// @return One on success or when no faces need processing; zero on allocation failure or overflow.
static int mesh3d_fill_missing_normals(rt_mesh3d *m) {
    double stack_accum[MESH3D_NORMAL_STACK_VERTS * 3u];
    double *accum;
    size_t accum_values;

    if (!m || m->index_count < 3u)
        return 1;
    if (!rt_alloc_count_ok(m->vertex_count, 3u * sizeof(double))) {
        rt_asset_error_set(RT_ASSET_ERROR_TOO_LARGE,
                           "Mesh3D.FromOBJ: normal accumulator allocation overflow");
        return 0;
    }
    accum_values = (size_t)m->vertex_count * 3u;
    accum = mesh3d_prepare_normal_accumulator(
        m, stack_accum, (size_t)MESH3D_NORMAL_STACK_VERTS * 3u, accum_values);
    if (!accum)
        return 0;
    for (uint32_t i = 0; i + 2 < m->index_count; i += 3) {
        uint32_t i0 = m->indices[i], i1 = m->indices[i + 1], i2 = m->indices[i + 2];
        if (i0 >= m->vertex_count || i1 >= m->vertex_count || i2 >= m->vertex_count)
            continue;
        float *p0 = m->vertices[i0].pos;
        float *p1 = m->vertices[i1].pos;
        float *p2 = m->vertices[i2].pos;
        double d0[3] = {(double)p0[0], (double)p0[1], (double)p0[2]};
        double d1[3] = {(double)p1[0], (double)p1[1], (double)p1[2]};
        double d2[3] = {(double)p2[0], (double)p2[1], (double)p2[2]};
        double e1[3] = {(double)p1[0] - (double)p0[0],
                        (double)p1[1] - (double)p0[1],
                        (double)p1[2] - (double)p0[2]};
        double e2[3] = {(double)p2[0] - (double)p0[0],
                        (double)p2[1] - (double)p0[1],
                        (double)p2[2] - (double)p0[2]};
        double nx = e1[1] * e2[2] - e1[2] * e2[1];
        double ny = e1[2] * e2[0] - e1[0] * e2[2];
        double nz = e1[0] * e2[1] - e1[1] * e2[0];
        double len_sq = nx * nx + ny * ny + nz * nz;
        if (!isfinite(len_sq) || len_sq <= mesh_triangle_area_epsilon_sq_f64(d0, d1, d2))
            continue;
        accum[(size_t)i0 * 3u + 0] += nx;
        accum[(size_t)i0 * 3u + 1] += ny;
        accum[(size_t)i0 * 3u + 2] += nz;
        accum[(size_t)i1 * 3u + 0] += nx;
        accum[(size_t)i1 * 3u + 1] += ny;
        accum[(size_t)i1 * 3u + 2] += nz;
        accum[(size_t)i2 * 3u + 0] += nx;
        accum[(size_t)i2 * 3u + 1] += ny;
        accum[(size_t)i2 * 3u + 2] += nz;
    }
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        float *n = m->vertices[i].normal;
        double existing_len = (double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2];
        if (isfinite(existing_len) && existing_len > 1e-20)
            continue;
        double nx = accum[(size_t)i * 3u + 0];
        double ny = accum[(size_t)i * 3u + 1];
        double nz = accum[(size_t)i * 3u + 2];
        double len = sqrt(nx * nx + ny * ny + nz * nz);
        if (isfinite(len) && len > 1e-12 && mesh_value_fits_float(nx / len) &&
            mesh_value_fits_float(ny / len) && mesh_value_fits_float(nz / len)) {
            n[0] = (float)(nx / len);
            n[1] = (float)(ny / len);
            n[2] = (float)(nz / len);
        } else {
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
        }
    }
    mesh3d_bump_vertex_revision(m, 1);
    return 1;
}

/// @brief Create a deep copy of a mesh (independent vertex/index arrays).
/// @details Copies and sanitizes authored geometry, submesh metadata, retained precision data,
///          skin maps, bounds, and residency flags. Skeletons are shared through a retained
///          reference, morph targets are cloned, and transient frame animation payloads and
///          acceleration caches are not copied.
/// @param obj Source Mesh3D handle.
/// @return New independent Mesh3D, or NULL for invalid/failed source state or allocation failure.
void *rt_mesh3d_clone(void *obj) {
    rt_mesh3d *src = mesh3d_checked(obj);
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t submesh_range_count;
    if (!src)
        return NULL;
    if (src->build_failed) {
        rt_trap("Mesh3D.Clone: cannot clone a failed mesh build");
        return NULL;
    }
    rt_mesh3d_repair_geometry_counts(src);
    if (!mesh3d_sanitize_triangle_indices(src, "Mesh3D.Clone: invalid triangle index buffer"))
        return NULL;
    vertex_count = rt_mesh3d_safe_vertex_count(src);
    index_count = rt_mesh3d_safe_index_count(src);
    submesh_range_count =
        (src->submesh_ranges && src->submesh_range_capacity > 0)
            ? (src->submesh_range_count < src->submesh_range_capacity ? src->submesh_range_count
                                                                      : src->submesh_range_capacity)
            : 0;
    rt_mesh3d *dst = (rt_mesh3d *)rt_mesh3d_new_empty_storage();
    if (!dst)
        return NULL;

    if (!rt_alloc_count_ok(vertex_count, sizeof(vgfx3d_vertex_t)) ||
        !rt_alloc_count_ok(index_count, sizeof(uint32_t)) ||
        !rt_alloc_count_ok(submesh_range_count, sizeof(rt_mesh3d_submesh_range)) ||
        (src->positions64 && !rt_alloc_count_ok(vertex_count, 3u * sizeof(double)))) {
        if (rt_obj_release_check0(dst))
            rt_obj_free(dst);
        rt_trap("Mesh3D.Clone: allocation overflow");
        return NULL;
    }

    dst->vertex_capacity = vertex_count;
    dst->vertices =
        dst->vertex_capacity > 0
            ? (vgfx3d_vertex_t *)malloc((size_t)dst->vertex_capacity * sizeof(vgfx3d_vertex_t))
            : NULL;
    if (src->positions64 && vertex_count > 0)
        dst->positions64 = (double *)malloc((size_t)dst->vertex_capacity * 3u * sizeof(double));
    dst->index_capacity = index_count;
    dst->indices = dst->index_capacity > 0
                       ? (uint32_t *)malloc((size_t)dst->index_capacity * sizeof(uint32_t))
                       : NULL;
    dst->submesh_range_capacity = submesh_range_count;
    dst->submesh_ranges = submesh_range_count > 0
                              ? (rt_mesh3d_submesh_range *)malloc((size_t)submesh_range_count *
                                                                  sizeof(rt_mesh3d_submesh_range))
                              : NULL;

    if ((dst->vertex_capacity > 0 && !dst->vertices) ||
        (dst->index_capacity > 0 && !dst->indices) ||
        (submesh_range_count > 0 && !dst->submesh_ranges) ||
        (src->positions64 && vertex_count > 0 && !dst->positions64)) {
        if (rt_obj_release_check0(dst))
            rt_obj_free(dst);
        rt_trap("Mesh3D.Clone: memory allocation failed");
        return NULL;
    }

    dst->vertex_count = vertex_count;
    if (vertex_count > 0) {
        memcpy(dst->vertices, src->vertices, (size_t)vertex_count * sizeof(vgfx3d_vertex_t));
        for (uint32_t i = 0; i < vertex_count; ++i)
            mesh3d_sanitize_vertex_copy(&dst->vertices[i]);
    }
    if (src->positions64 && vertex_count > 0) {
        memcpy(dst->positions64, src->positions64, (size_t)vertex_count * 3u * sizeof(double));
        for (uint32_t i = 0; i < vertex_count; ++i) {
            for (int lane = 0; lane < 3; ++lane) {
                size_t index = (size_t)i * 3u + (size_t)lane;
                if (!isfinite(dst->positions64[index]))
                    dst->positions64[index] = (double)dst->vertices[i].pos[lane];
            }
        }
    }

    dst->index_count = index_count;
    if (index_count > 0)
        memcpy(dst->indices, src->indices, (size_t)index_count * sizeof(uint32_t));
    dst->submesh_range_count = submesh_range_count;
    if (submesh_range_count > 0) {
        memcpy(dst->submesh_ranges,
               src->submesh_ranges,
               (size_t)submesh_range_count * sizeof(rt_mesh3d_submesh_range));
    }
    dst->simplify_requested_triangles = src->simplify_requested_triangles;
    dst->simplify_achieved_triangles = src->simplify_achieved_triangles;
    dst->simplify_status = src->simplify_status;
    dst->bone_palette = NULL;
    dst->prev_bone_palette = NULL;
    mesh_repair_animation_refs(src);
    dst->bone_count = (src->skeleton_ref || mesh3d_has_bone_weights(src)) ? src->bone_count : 0;
    if (src->bone_map && dst->bone_count > 0) {
        dst->bone_map = (int32_t *)malloc((size_t)dst->bone_count * sizeof(int32_t));
        if (dst->bone_map)
            memcpy(dst->bone_map, src->bone_map, (size_t)dst->bone_count * sizeof(int32_t));
    }
    if (src->extra_influences && dst->vertex_count > 0) {
        dst->extra_influences = (vgfx3d_extra_influences_t *)malloc(
            (size_t)dst->vertex_count * sizeof(vgfx3d_extra_influences_t));
        if (dst->extra_influences)
            memcpy(dst->extra_influences,
                   src->extra_influences,
                   (size_t)dst->vertex_count * sizeof(vgfx3d_extra_influences_t));
    }
    dst->morph_deltas = NULL;
    dst->morph_weights = NULL;
    dst->morph_shape_count = 0;
    dst->morph_normal_deltas = NULL;
    dst->prev_morph_weights = NULL;
    dst->morph_bound_deltas_source = NULL;
    dst->morph_bound_revision = 0;
    dst->morph_bound_vertex_count = 0;
    dst->morph_bound_shape_count = 0;
    dst->morph_bound_pad = 0.0;
    dst->morph_bound_valid = 0;
    mesh_assign_skeleton_ref(&dst->skeleton_ref, src->skeleton_ref);
    if (src->morph_targets_ref) {
        void *morph_clone = rt_morphtarget3d_clone(src->morph_targets_ref);
        if (!morph_clone) {
            if (rt_obj_release_check0(dst))
                rt_obj_free(dst);
            rt_trap("Mesh3D.Clone: morph target clone failed");
            return NULL;
        }
        mesh_assign_morph_ref(&dst->morph_targets_ref, morph_clone);
        if (rt_obj_release_check0(morph_clone))
            rt_obj_free(morph_clone);
    }
    dst->geometry_revision = src->geometry_revision;
    dst->tangent_revision = src->tangent_revision;
    dst->tangents_ready = src->tangents_ready;
    dst->validated_index_revision = 0;
    dst->validated_index_count = 0;
    dst->positions64_rebase_revision = src->positions64_rebase_revision;
    dst->positions64_rebase_needed = src->positions64_rebase_needed;
    dst->resident = src->resident ? 1 : 0;
    dst->compact_streams = src->compact_streams ? 1 : 0;
    dst->build_failed = 0;
    dst->aabb_min[0] = src->aabb_min[0];
    dst->aabb_min[1] = src->aabb_min[1];
    dst->aabb_min[2] = src->aabb_min[2];
    dst->aabb_max[0] = src->aabb_max[0];
    dst->aabb_max[1] = src->aabb_max[1];
    dst->aabb_max[2] = src->aabb_max[2];
    dst->bsphere_radius = src->bsphere_radius;
    dst->bounds_dirty = 1;
    rt_mesh3d_refresh_bounds(dst);

    return dst;
}

/// @brief Apply a 4x4 matrix to every vertex position, normal, and tangent in-place.
/// @details Positions are transformed by the full affine matrix. Normals and tangents
///          need the inverse-transpose of the upper-left 3x3 (the "normal matrix") so
///          non-uniform scales don't tilt them off the surface; `vgfx3d_compute_normal_matrix4`
///          computes that once and the per-vertex loop reuses it.
///
///          The matrix determinant is inspected for a handedness flip (det < 0): when
///          the transform mirrors the mesh, triangle winding is reversed to preserve
///          backface-culling semantics, and the tangent-space bitangent is negated to
///          keep the TBN frame right-handed. That tangent flip is encoded in the
///          tangent's W component (stored in `t[3]`) so the shader can reconstruct the
///          bitangent with `cross(N,T) * W`. We preserve any existing W (treating 0 as
///          the canonical +1 for legacy meshes), then multiply by the handedness sign.
///
///          Normals and tangents are renormalized after transform — shear / non-uniform
///          scale otherwise produce non-unit vectors. After the pass, geometry is
///          marked dirty and bounds are recomputed so downstream culling stays correct.
/// @param obj Target mesh (no-op when NULL).
/// @param mat4_obj Mat4 handle (no-op when NULL).
void rt_mesh3d_transform(void *obj, void *mat4_obj) {
    rt_mesh3d *m = mesh3d_checked(obj);
    mat4_impl *xform = mesh3d_mat4_checked(mat4_obj);
    uint32_t vertex_count;
    uint32_t index_count;
    if (!m || !xform)
        return;
    float model_matrix[16];
    float normal_matrix[16];
    float handedness_sign = 1.0f;
    double det;

    for (int i = 0; i < 16; i++) {
        if (!mesh_value_fits_float(xform->m[i])) {
            rt_trap("Mesh3D.Transform: matrix values must be finite and fit float range");
            return;
        }
        model_matrix[i] = (float)xform->m[i];
    }
    /* Evaluate the upper-3x3 determinant from the original double matrix: a
     * matrix that is singular in double precision can still show |det| ~1e-7
     * once narrowed to float, which would slip past the invertibility gate. */
    det = xform->m[0] * (xform->m[5] * xform->m[10] - xform->m[6] * xform->m[9]) -
          xform->m[1] * (xform->m[4] * xform->m[10] - xform->m[6] * xform->m[8]) +
          xform->m[2] * (xform->m[4] * xform->m[9] - xform->m[5] * xform->m[8]);
    if (!isfinite(det) || fabs(det) <= 1e-12) {
        rt_trap("Mesh3D.Transform: matrix upper 3x3 must be invertible for normal transform");
        return;
    }
    vgfx3d_compute_normal_matrix4(model_matrix, normal_matrix);
    if (!mesh_matrix4f_is_finite(normal_matrix)) {
        rt_trap("Mesh3D.Transform: normal matrix must be finite");
        return;
    }
    if (det < 0.0)
        handedness_sign = -1.0f;

    rt_mesh3d_repair_geometry_counts(m);
    vertex_count = rt_mesh3d_safe_vertex_count(m);
    index_count = rt_mesh3d_safe_index_count(m);

    for (uint32_t i = 0; i < vertex_count; i++) {
        double x =
            m->positions64 ? m->positions64[(size_t)i * 3u + 0] : (double)m->vertices[i].pos[0];
        double y =
            m->positions64 ? m->positions64[(size_t)i * 3u + 1] : (double)m->vertices[i].pos[1];
        double z =
            m->positions64 ? m->positions64[(size_t)i * 3u + 2] : (double)m->vertices[i].pos[2];
        double tx = xform->m[0] * x + xform->m[1] * y + xform->m[2] * z + xform->m[3];
        double ty = xform->m[4] * x + xform->m[5] * y + xform->m[6] * z + xform->m[7];
        double tz = xform->m[8] * x + xform->m[9] * y + xform->m[10] * z + xform->m[11];
        if (!mesh_value_fits_float(tx) || !mesh_value_fits_float(ty) ||
            !mesh_value_fits_float(tz)) {
            rt_trap(
                "Mesh3D.Transform: transformed vertex position must be finite and fit float range");
            return;
        }
        {
            const float *n = m->vertices[i].normal;
            const float *t = m->vertices[i].tangent;
            double nx = normal_matrix[0] * (double)n[0] + normal_matrix[1] * (double)n[1] +
                        normal_matrix[2] * (double)n[2];
            double ny = normal_matrix[4] * (double)n[0] + normal_matrix[5] * (double)n[1] +
                        normal_matrix[6] * (double)n[2];
            double nz = normal_matrix[8] * (double)n[0] + normal_matrix[9] * (double)n[1] +
                        normal_matrix[10] * (double)n[2];
            double txv = normal_matrix[0] * (double)t[0] + normal_matrix[1] * (double)t[1] +
                         normal_matrix[2] * (double)t[2];
            double tyv = normal_matrix[4] * (double)t[0] + normal_matrix[5] * (double)t[1] +
                         normal_matrix[6] * (double)t[2];
            double tzv = normal_matrix[8] * (double)t[0] + normal_matrix[9] * (double)t[1] +
                         normal_matrix[10] * (double)t[2];
            if (!mesh_value_fits_float(nx) || !mesh_value_fits_float(ny) ||
                !mesh_value_fits_float(nz) || !mesh_value_fits_float(txv) ||
                !mesh_value_fits_float(tyv) || !mesh_value_fits_float(tzv)) {
                rt_trap("Mesh3D.Transform: transformed normal/tangent must be finite and fit float "
                        "range");
                return;
            }
        }
    }

    for (uint32_t i = 0; i < vertex_count; i++) {
        float *p = m->vertices[i].pos;
        double x = m->positions64 ? m->positions64[(size_t)i * 3u + 0] : (double)p[0];
        double y = m->positions64 ? m->positions64[(size_t)i * 3u + 1] : (double)p[1];
        double z = m->positions64 ? m->positions64[(size_t)i * 3u + 2] : (double)p[2];
        double px = xform->m[0] * x + xform->m[1] * y + xform->m[2] * z + xform->m[3];
        double py = xform->m[4] * x + xform->m[5] * y + xform->m[6] * z + xform->m[7];
        double pz = xform->m[8] * x + xform->m[9] * y + xform->m[10] * z + xform->m[11];
        if (m->positions64) {
            m->positions64[(size_t)i * 3u + 0] = px;
            m->positions64[(size_t)i * 3u + 1] = py;
            m->positions64[(size_t)i * 3u + 2] = pz;
        }
        p[0] = (float)px;
        p[1] = (float)py;
        p[2] = (float)pz;

        /* Transform normals with the inverse-transpose upper 3x3. Keep the
         * source components in float (matching the tangent path below) instead
         * of bouncing through double and back. */
        float *n = m->vertices[i].normal;
        float nx = n[0], ny = n[1], nz = n[2];
        n[0] = normal_matrix[0] * nx + normal_matrix[1] * ny + normal_matrix[2] * nz;
        n[1] = normal_matrix[4] * nx + normal_matrix[5] * ny + normal_matrix[6] * nz;
        n[2] = normal_matrix[8] * nx + normal_matrix[9] * ny + normal_matrix[10] * nz;
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (isfinite(len) && len > 1e-8f) {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        } else {
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
        }

        float *t = m->vertices[i].tangent;
        float handedness = (!isfinite(t[3]) || t[3] == 0.0f) ? 1.0f : (t[3] < 0.0f ? -1.0f : 1.0f);
        float tx = t[0];
        float ty = t[1];
        float tz = t[2];
        t[0] = normal_matrix[0] * tx + normal_matrix[1] * ty + normal_matrix[2] * tz;
        t[1] = normal_matrix[4] * tx + normal_matrix[5] * ty + normal_matrix[6] * tz;
        t[2] = normal_matrix[8] * tx + normal_matrix[9] * ty + normal_matrix[10] * tz;
        len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (isfinite(len) && len > 1e-8f) {
            t[0] /= len;
            t[1] /= len;
            t[2] /= len;
        } else {
            mesh_default_tangent_from_normal(n, t);
        }
        t[3] = handedness * handedness_sign;
    }
    if (handedness_sign < 0.0f) {
        for (uint32_t i = 0; i + 2 < index_count; i += 3) {
            uint32_t tmp = m->indices[i + 1];
            m->indices[i + 1] = m->indices[i + 2];
            m->indices[i + 2] = tmp;
        }
    }
    rt_mesh3d_touch_geometry(m);
    rt_mesh3d_refresh_bounds(m);
}

#include "rt_mesh3d_obj.inc"
#include "rt_mesh3d_procedural.inc"
#include "rt_mesh3d_stl.inc"
#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
