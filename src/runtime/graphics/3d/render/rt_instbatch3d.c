//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_instbatch3d.c
// Purpose: Instanced rendering batch — stores N transforms for one mesh+material.
//   Canvas3D.DrawInstanced culls per-instance and dispatches via backend.
//
// Key invariants:
//   - Transforms are retained as double[16] row-major plus float submit mirrors.
//   - Software backend: loops N individual submit_draw calls.
//   - Mesh/material are retained by the batch because it stores them across frames.
//   - The legacy float buffer layout is preserved for tests/tools that inspect
//     batch internals; the double transform buffer is appended after those fields.
//   - Four parallel transform buffers are kept: authoritative double transforms,
//     live float mirrors, start-of-frame snapshots, and previous-frame transforms.
//
// Ownership/Lifetime:
//   - InstanceBatch3D is GC-managed; finalizer releases mesh, material,
//     and the matrix buffers.
//   - Transient per-frame matrix copies are parked on the canvas's temp-buffer
//     queue and freed at end-of-frame.
//
// Links: rt_instbatch3d.h, rt_canvas3d.c, vgfx3d_backend.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements retained multi-transform InstanceBatch3D rendering.
/// @details Batches preserve double-precision authoritative transforms, maintain
///   sanitized float mirrors and motion history, repair private state, perform
///   per-instance frustum culling, and queue stable instanced submissions.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_instbatch3d.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_mat4.h"
#include "rt_object.h"
#include "vgfx3d_backend.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern void rt_obj_set_finalizer(void *obj, void (*fn)(void *));
extern void rt_obj_retain_maybe(void *obj);
extern int rt_obj_release_check0(void *obj);
extern void rt_obj_free(void *obj);
#include "rt_trap.h"
extern double rt_mat4_get(void *m, int64_t r, int64_t c);

#define INST_INIT_CAP 64
#define INSTBATCH3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define INSTBATCH3D_WORLD_ABS_MAX 1000000000000.0

typedef struct {
    void *vptr;
    void *mesh;              /* retained Mesh3D */
    void *material;          /* retained Material3D */
    float *transforms;       /* N * 16 floats */
    float *current_snapshot; /* current-frame snapshot for motion history */
    float *prev_transforms;  /* previous-frame transforms */
    int32_t instance_count;
    int32_t instance_capacity;
    int32_t motion_snapshot_count;
    int32_t prev_count;
    int64_t last_motion_frame;
    int8_t has_prev_snapshot;
    double *transforms64; /* authoritative N * 16 double matrices */
    float *visible_transforms;
    float *visible_prev_transforms;
    int32_t visible_capacity;
    int32_t visible_prev_capacity;
    float *prev_submit_transforms;
    int32_t prev_submit_capacity;
} rt_instbatch3d;

/// @brief Compute the next geometric growth capacity for the instance buffer.
/// @details Starting capacity is INST_INIT_CAP; subsequent doublings are guarded
///   against INT32_MAX/2 overflow and against the resulting byte-count exceeding
///   SIZE_MAX so both signed-integer and size_t overflow are prevented.  Returns 1
///   on success and writes the new capacity into *out_capacity; returns 0 if the
///   current capacity is already too large to double safely.
/// @param current Current matrix-slot capacity; nonpositive values select the initial capacity.
/// @param out_capacity Output receiving the proposed positive capacity.
/// @return Nonzero when a safe capacity was written; zero for a null output or overflow.
static int instbatch_next_capacity(int32_t current, int32_t *out_capacity) {
    if (!out_capacity)
        return 0;
    if (current <= 0) {
        *out_capacity = INST_INIT_CAP;
        return 1;
    }
    if (current > INT32_MAX / 2)
        return 0;
    int32_t next = current * 2;
    if ((size_t)next > SIZE_MAX / (16u * sizeof(double)))
        return 0;
    *out_capacity = next;
    return 1;
}

/// @brief Return the (row, col) element of a 4x4 identity matrix.
/// @details Used as a NaN/Inf fallback value when sanitizing incoming Mat4
///   transforms — replacing bad floats with the identity element preserves the
///   matrix structure and avoids propagating undefined GPU state.
/// @param row Zero-based matrix row.
/// @param col Zero-based matrix column.
/// @return `1.0` on the diagonal and `0.0` elsewhere.
static float instbatch_identity_at(int row, int col) {
    return row == col ? 1.0f : 0.0f;
}

/// @brief True if `value` is finite and within ±INSTBATCH3D_FLOAT_ABS_MAX (safe to narrow to
/// float).
/// @param value Candidate double-precision matrix element.
/// @return Nonzero when conversion to finite `float` cannot overflow.
static int instbatch_value_fits_float(double value) {
    return isfinite(value) && value >= -INSTBATCH3D_FLOAT_ABS_MAX &&
           value <= INSTBATCH3D_FLOAT_ABS_MAX;
}

/// @brief Sanitize a matrix element before storing/submitting it as instance state.
/// @param value Candidate element.
/// @param row Element row used to select an identity fallback.
/// @param col Element column used to select an identity fallback.
/// @return Finite float clamped to the supported world range, or the matching
///   identity element when narrowing is unsafe.
static float instbatch_sanitize_matrix_value(double value, int row, int col) {
    if (!instbatch_value_fits_float(value))
        return instbatch_identity_at(row, col);
    if (value > INSTBATCH3D_WORLD_ABS_MAX)
        return (float)INSTBATCH3D_WORLD_ABS_MAX;
    if (value < -INSTBATCH3D_WORLD_ABS_MAX)
        return (float)-INSTBATCH3D_WORLD_ABS_MAX;
    return (float)value;
}

/// @brief Sanitize a matrix element while keeping it in double precision.
/// @details Values that cannot later be narrowed to the backend float matrix
///   representation are treated like non-finite values and replaced with the
///   identity element for their row/column. Finite values inside float range
///   but outside the supported world bound are clamped so large translations
///   remain usable without poisoning raster state.
/// @param value Candidate element.
/// @param row Element row used to select an identity fallback.
/// @param col Element column used to select an identity fallback.
/// @return Sanitized finite double suitable for authoritative storage.
static double instbatch_sanitize_matrix_value64(double value, int row, int col) {
    if (!instbatch_value_fits_float(value))
        return (double)instbatch_identity_at(row, col);
    if (value > INSTBATCH3D_WORLD_ABS_MAX)
        return INSTBATCH3D_WORLD_ABS_MAX;
    if (value < -INSTBATCH3D_WORLD_ABS_MAX)
        return -INSTBATCH3D_WORLD_ABS_MAX;
    return value;
}

/// @brief True if `transform` is a live Mat4 runtime instance of the expected size.
/// @param transform Candidate runtime object handle.
/// @return Nonzero when the object has the Mat4 class and implementation size.
static int instbatch_mat4_valid(void *transform) {
    return transform && rt_obj_is_instance(transform, RT_MAT4_CLASS_ID, sizeof(mat4_impl));
}

/// @brief Copy a Mat4 object into a double[16] slot, preserving precision until submission.
/// @param dst Output array of sixteen doubles; `NULL` is ignored.
/// @param transform Borrowed validated Mat4 receiver read element-by-element.
static void instbatch_copy_mat4_sanitized64(double *dst, void *transform) {
    if (!dst)
        return;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double value = rt_mat4_get(transform, i, j);
            dst[i * 4 + j] = instbatch_sanitize_matrix_value64(value, i, j);
        }
    }
}

/// @brief Narrow one authoritative double matrix slot into its float submit mirror.
/// @param dst Output array of sixteen floats.
/// @param src Input array of sixteen authoritative doubles.
static void instbatch_copy_matrix64_to_float(float *dst, const double *src) {
    if (!dst || !src)
        return;
    for (int i = 0; i < 16; i++)
        dst[i] = instbatch_sanitize_matrix_value(src[i], i / 4, i % 4);
}

/// @brief Replace invalid/extreme floats in a stored matrix slot with bounded values.
/// @param slot In/out array of sixteen float matrix elements; `NULL` is ignored.
static void instbatch_sanitize_matrix_slot(float *slot) {
    if (!slot)
        return;
    for (int i = 0; i < 16; i++)
        slot[i] = instbatch_sanitize_matrix_value(slot[i], i / 4, i % 4);
}

/// @brief Repair all active matrix slots before motion snapshots or backend submission.
/// @details Regenerates live float mirrors from authoritative doubles when
///   available and sanitizes current/previous motion snapshots in place.
/// @param b Batch whose active matrix ranges are repaired; `NULL` is ignored.
static void instbatch_sanitize_active_matrices(rt_instbatch3d *b) {
    if (!b)
        return;
    for (int32_t i = 0; i < b->instance_count; i++) {
        if (b->transforms64)
            instbatch_copy_matrix64_to_float(&b->transforms[(size_t)i * 16u],
                                             &b->transforms64[(size_t)i * 16u]);
        else
            instbatch_sanitize_matrix_slot(&b->transforms[(size_t)i * 16u]);
    }
    for (int32_t i = 0; i < b->motion_snapshot_count; i++)
        instbatch_sanitize_matrix_slot(&b->current_snapshot[(size_t)i * 16u]);
    for (int32_t i = 0; i < b->prev_count; i++)
        instbatch_sanitize_matrix_slot(&b->prev_transforms[(size_t)i * 16u]);
}

/// @brief Copy one 4x4 matrix between stride-16-float slots.
/// @details Instance batches store transforms in a flat float array where
///   slot `i` occupies `[i*16, i*16+16)`. This helper encapsulates the
///   stride math so callers don't repeat it across the file, and folds in
///   the null / negative-index guards that the motion-snapshot path
///   depends on.
/// @param dst Destination flat matrix array.
/// @param dst_idx Nonnegative destination matrix index.
/// @param src Source flat matrix array; may alias @p dst.
/// @param src_idx Nonnegative source matrix index.
static void instbatch_copy_matrix_slot(float *dst,
                                       int32_t dst_idx,
                                       const float *src,
                                       int32_t src_idx) {
    if (!dst || !src || dst_idx < 0 || src_idx < 0)
        return;
    memcpy(&dst[(size_t)dst_idx * 16u], &src[(size_t)src_idx * 16u], 16u * sizeof(float));
    instbatch_sanitize_matrix_slot(&dst[(size_t)dst_idx * 16u]);
}

/// @brief Ensure a retained float-matrix scratch buffer can hold @p needed matrices.
/// @details InstanceBatch draw uses this for partial-cull and previous-transform repair paths.
///   The queueing layer snapshots submitted matrices before returning, so these buffers can be
///   retained on the batch and reused across frames without frame-temp ownership.
/// @param slot In/out scratch pointer.
/// @param capacity In/out matrix capacity for @p slot.
/// @param needed Number of 4x4 matrices required.
/// @return Non-zero when the scratch buffer is available.
static int instbatch_ensure_matrix_scratch(float **slot, int32_t *capacity, int32_t needed) {
    float *grown;
    int32_t new_capacity;

    if (!slot || !capacity || needed < 0)
        return 0;
    if (needed == 0)
        return 1;
    if (*capacity >= needed && *slot)
        return 1;
    new_capacity = *capacity > 0 ? *capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / (16u * sizeof(float)))
        return 0;
    grown = (float *)realloc(*slot, (size_t)new_capacity * 16u * sizeof(*grown));
    if (!grown)
        return 0;
    *slot = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Convert a double matrix to the active canvas frame's float render space.
/// @details Camera-relative canvases subtract the current origin before narrowing translation, so
///          large world coordinates retain precision without decomposing the batch into individual
///          mesh draws. Non-translation terms are still validated against the float backend range.
/// @param c Canvas providing the current camera-relative origin.
/// @param src Row-major double 4x4 source matrix.
/// @param dst Row-major float 4x4 destination matrix.
/// @return Non-zero when every element can be represented safely.
static int instbatch_matrix64_to_canvas_frame(const rt_canvas3d *c, const double *src, float *dst) {
    if (!src || !dst)
        return 0;
    for (int i = 0; i < 16; i++) {
        double value = src[i];
        if (!isfinite(value))
            return 0;
        if (canvas3d_uses_camera_relative_upload(c)) {
            if (i == 3)
                value -= c->camera_relative_origin[0];
            else if (i == 7)
                value -= c->camera_relative_origin[1];
            else if (i == 11)
                value -= c->camera_relative_origin[2];
        }
        if (!instbatch_value_fits_float(value))
            return 0;
        dst[i] = (float)value;
    }
    return 1;
}

/// @brief Drop a retained reference from a slot and clear it.
/// @details Paired helper for the batch's mesh / material slots — the
///   instance-batch owns refs to these, so finalize must release them. Idempotent on
///   already-null slots so a partially-initialized batch can be torn down safely.
/// @param slot Address of an owned runtime-reference slot.
static void instbatch_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained Mesh3D slot only when it still points at Mesh3D.
/// @param slot Address of a possibly owned mesh slot. A stale non-null value is
///   cleared without attempting reference-count release.
static void instbatch_release_mesh_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MESH3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    instbatch_release_ref(slot);
}

/// @brief Release a retained Material3D slot only when it still points at Material3D.
/// @param slot Address of a possibly owned material slot. A stale non-null value
///   is cleared without attempting reference-count release.
static void instbatch_release_material_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MATERIAL3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    instbatch_release_ref(slot);
}

/// @brief Release and clear corrupted private resource slots.
/// @param b Batch whose retained mesh and material handles are repaired;
///   `NULL` is ignored.
static void instbatch_repair_resource_handles(rt_instbatch3d *b) {
    if (!b)
        return;
    if (b->mesh && !rt_g3d_has_class(b->mesh, RT_G3D_MESH3D_CLASS_ID))
        instbatch_release_mesh_slot(&b->mesh);
    if (b->material && !rt_g3d_has_class(b->material, RT_G3D_MATERIAL3D_CLASS_ID))
        instbatch_release_material_slot(&b->material);
}

/// @brief Repair count/buffer invariants before mutating or drawing a batch.
/// @details Reconstructs a missing authoritative double mirror when legacy float
///   buffers are intact. Otherwise it discards inconsistent arrays and creates
///   a fresh empty initial-capacity set. Counts are clamped to live capacities,
///   and motion-history flags are normalized.
/// @param b Batch to repair in place.
/// @return Nonzero when four coherent primary matrix buffers are available;
///   zero for null input or reconstruction/allocation failure.
static int instbatch_repair_state(rt_instbatch3d *b) {
    if (!b)
        return 0;
    instbatch_repair_resource_handles(b);
    if (b->instance_capacity > 0 && b->transforms && b->current_snapshot && b->prev_transforms &&
        !b->transforms64) {
        b->transforms64 = (double *)calloc((size_t)b->instance_capacity * 16u, sizeof(double));
        if (!b->transforms64)
            return 0;
        for (int32_t i = 0; i < b->instance_count; i++)
            for (int j = 0; j < 16; j++)
                b->transforms64[(size_t)i * 16u + (size_t)j] =
                    (double)b->transforms[(size_t)i * 16u + (size_t)j];
    }
    if (b->instance_capacity <= 0 || !b->transforms || !b->current_snapshot ||
        !b->prev_transforms || !b->transforms64) {
        free(b->transforms);
        free(b->current_snapshot);
        free(b->prev_transforms);
        free(b->transforms64);
        b->transforms64 = (double *)calloc(INST_INIT_CAP * 16, sizeof(double));
        b->transforms = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
        b->current_snapshot = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
        b->prev_transforms = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
        b->instance_count = 0;
        b->motion_snapshot_count = 0;
        b->prev_count = 0;
        b->has_prev_snapshot = 0;
        b->instance_capacity =
            (b->transforms64 && b->transforms && b->current_snapshot && b->prev_transforms)
                ? INST_INIT_CAP
                : 0;
        if (b->instance_capacity == 0) {
            free(b->transforms64);
            free(b->transforms);
            free(b->current_snapshot);
            free(b->prev_transforms);
            b->transforms64 = NULL;
            b->transforms = NULL;
            b->current_snapshot = NULL;
            b->prev_transforms = NULL;
        }
        return b->instance_capacity > 0;
    }
    if (b->instance_count < 0)
        b->instance_count = 0;
    if (b->instance_count > b->instance_capacity)
        b->instance_count = b->instance_capacity;
    if (b->motion_snapshot_count < 0)
        b->motion_snapshot_count = 0;
    if (b->motion_snapshot_count > b->instance_count)
        b->motion_snapshot_count = b->instance_count;
    if (b->prev_count < 0)
        b->prev_count = 0;
    if (b->prev_count > b->instance_count)
        b->prev_count = b->instance_count;
    b->has_prev_snapshot = (b->has_prev_snapshot && b->prev_count > 0) ? 1 : 0;
    return 1;
}

/// @brief Per-instance frustum cull test for an instanced batch.
/// @details Transforms the mesh's local AABB by one instance's model matrix
///   into world space, then runs the standard p-vertex/n-vertex frustum
///   test. Returns visible (1) when the frustum pointer is null so disabling
///   culling at a higher level (e.g. shadow pass without a camera frustum)
///   doesn't accidentally hide every instance. The matrix is promoted to
///   double precision for the transform because the AABB refit can amplify
///   rounding at large world coordinates.
/// @param frustum Borrowed camera frustum; `NULL` disables rejection.
/// @param mesh_min Three-element local-space minimum AABB corner.
/// @param mesh_max Three-element local-space maximum AABB corner.
/// @param model_matrix Sixteen-element row-major instance transform.
/// @return 1 if the instance's world AABB is on-screen or intersecting, 0 if
///   definitively outside the frustum.
static int instbatch_instance_visible(const vgfx3d_frustum_t *frustum,
                                      const float mesh_min[3],
                                      const float mesh_max[3],
                                      const float *model_matrix) {
    double world_matrix[16];
    float world_min[3];
    float world_max[3];

    if (!frustum || !mesh_min || !mesh_max || !model_matrix)
        return 1;

    for (int i = 0; i < 16; i++)
        world_matrix[i] = (double)model_matrix[i];
    if (!vgfx3d_transform_aabb_checked(mesh_min, mesh_max, world_matrix, world_min, world_max))
        return 1;
    return vgfx3d_frustum_test_aabb(frustum, world_min, world_max) != 0;
}

/// @brief GC finalizer — release the current-frame and motion-history buffers.
/// @details Instance batches keep one authoritative double-matrix array plus
///   three parallel float-matrix arrays: the live `transforms` mirror, the
///   start-of-frame `current_snapshot` captured by motion-vector logic, and
///   last frame's `prev_transforms` used as the motion "from" pose. All four
///   are plain heap allocations with no downstream refs to release. Counters are zeroed post-free
///   so a lingering post-finalize read sees an empty batch rather than
///   capacity-matches-missing-buffer.
/// @param obj InstanceBatch3D payload being finalized; `NULL` is ignored.
static void instbatch_finalizer(void *obj) {
    rt_instbatch3d *b = (rt_instbatch3d *)obj;
    if (!b)
        return;
    free(b->transforms);
    free(b->transforms64);
    free(b->current_snapshot);
    free(b->prev_transforms);
    free(b->visible_transforms);
    free(b->visible_prev_transforms);
    free(b->prev_submit_transforms);
    b->transforms = NULL;
    b->transforms64 = NULL;
    b->current_snapshot = NULL;
    b->prev_transforms = NULL;
    b->visible_transforms = NULL;
    b->visible_prev_transforms = NULL;
    b->prev_submit_transforms = NULL;
    b->instance_count = b->instance_capacity = 0;
    b->visible_capacity = b->visible_prev_capacity = b->prev_submit_capacity = 0;
    b->motion_snapshot_count = b->prev_count = 0;
    b->last_motion_frame = 0;
    b->has_prev_snapshot = 0;
    instbatch_release_mesh_slot(&b->mesh);
    instbatch_release_material_slot(&b->material);
}

/// @brief Create an instance batch for drawing N copies of one mesh efficiently.
/// @details Instance batching draws many objects with the same mesh and material
///          but different transforms in fewer draw calls. The software backend
///          falls back to individual draws; GPU backends may use native instancing.
///          Transforms are stored as contiguous double[16*N] row-major Mat4 arrays with float
///          mirrors generated for backend submission.
/// @param mesh     Mesh handle shared by all instances. The batch retains it
///                 because it is reused across frames.
/// @param material Material handle shared by all instances. The batch retains
///                 it because it is reused across frames.
/// @return Opaque batch handle, or NULL on failure.
void *rt_instbatch3d_new(void *mesh, void *material) {
    mesh = rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    material = rt_g3d_checked_or_null(material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (!mesh || !material)
        return NULL;
    rt_instbatch3d *b = (rt_instbatch3d *)rt_obj_new_i64(RT_G3D_INSTANCEBATCH3D_CLASS_ID,
                                                         (int64_t)sizeof(rt_instbatch3d));
    if (!b) {
        rt_trap("InstanceBatch3D.New: allocation failed");
        return NULL;
    }
    b->vptr = NULL;
    b->mesh = mesh;
    b->material = material;
    rt_obj_retain_maybe(mesh);
    rt_obj_retain_maybe(material);
    b->transforms64 = (double *)calloc(INST_INIT_CAP * 16, sizeof(double));
    b->transforms = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
    b->current_snapshot = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
    b->prev_transforms = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
    b->instance_count = 0;
    b->instance_capacity = INST_INIT_CAP;
    b->motion_snapshot_count = 0;
    b->prev_count = 0;
    b->last_motion_frame = 0;
    b->has_prev_snapshot = 0;
    if (!b->transforms64 || !b->transforms || !b->current_snapshot || !b->prev_transforms) {
        instbatch_finalizer(b);
        if (rt_obj_release_check0(b))
            rt_obj_free(b);
        rt_trap("InstanceBatch3D.New: allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(b, instbatch_finalizer);
    return b;
}

/// @brief Add an instance with the given transform (grows array if full).
/// @details Copies and sanitizes the Mat4 into authoritative double storage and
///   its float submit mirror. All four parallel arrays grow transactionally and
///   geometrically; allocation or overflow failure reports a trap without
///   incrementing the instance count.
/// @param obj InstanceBatch3D receiver; invalid handles are ignored.
/// @param transform Valid Mat4 to copy; ownership remains with the caller.
void rt_instbatch3d_add(void *obj, void *transform) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!b || !instbatch_mat4_valid(transform))
        return;
    if (!instbatch_repair_state(b)) {
        rt_trap("InstanceBatch3D.Add: allocation failed");
        return;
    }

    if (b->instance_count >= b->instance_capacity) {
        int32_t new_cap;
        if (!instbatch_next_capacity(b->instance_capacity, &new_cap)) {
            rt_trap("InstanceBatch3D.Add: instance capacity overflow");
            return;
        }
        size_t old_bytes = (size_t)b->instance_capacity * 16u * sizeof(float);
        size_t new_bytes = (size_t)new_cap * 16u * sizeof(float);
        size_t old_bytes64 = (size_t)b->instance_capacity * 16u * sizeof(double);
        size_t new_bytes64 = (size_t)new_cap * 16u * sizeof(double);
        double *new_transforms64 = (double *)malloc(new_bytes64);
        float *new_transforms = (float *)malloc(new_bytes);
        float *new_snapshot = (float *)malloc(new_bytes);
        float *new_prev = (float *)malloc(new_bytes);
        if (!new_transforms64 || !new_transforms || !new_snapshot || !new_prev) {
            free(new_transforms64);
            free(new_transforms);
            free(new_snapshot);
            free(new_prev);
            rt_trap("InstanceBatch3D.Add: allocation failed");
            return;
        }
        if (old_bytes > 0) {
            memcpy(new_transforms64, b->transforms64, old_bytes64);
            memcpy(new_transforms, b->transforms, old_bytes);
            memcpy(new_snapshot, b->current_snapshot, old_bytes);
            memcpy(new_prev, b->prev_transforms, old_bytes);
        }
        free(b->transforms64);
        free(b->transforms);
        free(b->current_snapshot);
        free(b->prev_transforms);
        b->transforms64 = new_transforms64;
        b->transforms = new_transforms;
        b->current_snapshot = new_snapshot;
        b->prev_transforms = new_prev;
        b->instance_capacity = new_cap;
    }

    double *dst64 = &b->transforms64[(size_t)b->instance_count * 16u];
    float *dst = &b->transforms[(size_t)b->instance_count * 16u];
    instbatch_copy_mat4_sanitized64(dst64, transform);
    instbatch_copy_matrix64_to_float(dst, dst64);

    b->instance_count++;
}

/// @brief Remove an instance by index (swap-removes with last for O(1) time).
/// @details Moves the final authoritative/live transform into the removed slot
///   and repairs current/previous snapshot counts so motion arrays retain
///   positional correspondence. Ordering is not preserved.
/// @param obj InstanceBatch3D receiver; invalid handles are ignored.
/// @param index Zero-based instance index; out-of-range values are ignored.
void rt_instbatch3d_remove(void *obj, int64_t index) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!b)
        return;
    if (!instbatch_repair_state(b))
        return;
    int32_t last_idx;
    if (index < 0 || index >= b->instance_count)
        return;
    last_idx = b->instance_count - 1;

    /* Swap with last */
    if (index < last_idx) {
        instbatch_copy_matrix_slot(b->transforms, (int32_t)index, b->transforms, last_idx);
        memcpy(&b->transforms64[(size_t)index * 16u],
               &b->transforms64[(size_t)last_idx * 16u],
               16u * sizeof(double));
        if (last_idx < b->motion_snapshot_count)
            instbatch_copy_matrix_slot(
                b->current_snapshot, (int32_t)index, b->current_snapshot, last_idx);
        else if (index < b->motion_snapshot_count)
            b->motion_snapshot_count = (int32_t)index;
        if (last_idx < b->prev_count)
            instbatch_copy_matrix_slot(
                b->prev_transforms, (int32_t)index, b->prev_transforms, last_idx);
        else if (index < b->prev_count) {
            b->prev_count = (int32_t)index;
            b->has_prev_snapshot = b->prev_count > 0 ? 1 : 0;
        }
    }
    b->instance_count--;
    if (b->motion_snapshot_count > b->instance_count)
        b->motion_snapshot_count = b->instance_count;
    if (b->prev_count > b->instance_count)
        b->prev_count = b->instance_count;
}

/// @brief Update the transform of an existing instance at the given index.
/// @param obj InstanceBatch3D receiver; invalid handles are ignored.
/// @param index Zero-based instance index; out-of-range values are ignored.
/// @param transform Valid Mat4 copied into authoritative and float mirror storage.
void rt_instbatch3d_set(void *obj, int64_t index, void *transform) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!b || !instbatch_mat4_valid(transform))
        return;
    if (!instbatch_repair_state(b))
        return;
    if (index < 0 || index >= b->instance_count)
        return;

    double *dst64 = &b->transforms64[(size_t)index * 16u];
    float *dst = &b->transforms[(size_t)index * 16u];
    instbatch_copy_mat4_sanitized64(dst64, transform);
    instbatch_copy_matrix64_to_float(dst, dst64);
}

/// @brief Remove all instances from the batch, resetting count to zero.
/// @details Retains allocated matrix capacity for reuse and discards all current
///   and previous motion-history counts.
/// @param obj InstanceBatch3D receiver; invalid handles are ignored.
void rt_instbatch3d_clear(void *obj) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!b)
        return;
    (void)instbatch_repair_state(b);
    b->instance_count = 0;
    b->motion_snapshot_count = 0;
    b->prev_count = 0;
    b->has_prev_snapshot = 0;
}

/// @brief Get the current number of instances in the batch.
/// @param obj InstanceBatch3D receiver.
/// @return Repaired nonnegative instance count, or zero for an invalid or
///   unrecoverable batch.
int64_t rt_instbatch3d_count(void *obj) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!b || !instbatch_repair_state(b))
        return 0;
    return b->instance_count;
}

/// @brief `InstanceBatch3D.GetTransform(index)` — read one instance back (ADR 0227).
/// @details Copies the authoritative double-precision matrix, so a value set
///          through `Set` reads back exactly.
/// @param obj InstanceBatch3D receiver.
/// @param index Zero-based instance index.
/// @return New Mat4 copy, or identity for invalid handles or out-of-range indices.
void *rt_instbatch3d_get(void *obj, int64_t index) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!b || !instbatch_repair_state(b) || !b->transforms64 || index < 0 ||
        index >= b->instance_count)
        return rt_mat4_identity();
    {
        const double *m = b->transforms64 + (size_t)index * 16u;
        return rt_mat4_new(m[0],
                           m[1],
                           m[2],
                           m[3],
                           m[4],
                           m[5],
                           m[6],
                           m[7],
                           m[8],
                           m[9],
                           m[10],
                           m[11],
                           m[12],
                           m[13],
                           m[14],
                           m[15]);
    }
}

/// @brief Internal bridge: borrow the batch's retained mesh (NULL for invalid handles).
/// @param batch InstanceBatch3D receiver.
/// @return Borrowed live Mesh3D handle, or `NULL`; the caller must not release it.
void *rt_instbatch3d_borrow_mesh(void *batch) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(batch, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    return b ? rt_g3d_checked_or_null(b->mesh, RT_G3D_MESH3D_CLASS_ID) : NULL;
}

/// @brief Internal bridge: borrow the batch's retained material (NULL for invalid handles).
/// @param batch InstanceBatch3D receiver.
/// @return Borrowed live Material3D handle, or `NULL`; the caller must not release it.
void *rt_instbatch3d_borrow_material(void *batch) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(batch, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    return b ? rt_g3d_checked_or_null(b->material, RT_G3D_MATERIAL3D_CLASS_ID) : NULL;
}

/// @brief Internal bridge: borrow the batch's float transform array (N * 16, sanitized).
/// @details Repairs batch state and regenerates/sanitizes active float mirrors
///   before exposing them. The pointer remains batch-owned and can be invalidated
///   by later mutation, growth, repair, or finalization.
/// @param batch InstanceBatch3D receiver.
/// @param out_count Optional output initialized to zero and set to the matrix
///   count on success.
/// @return Borrowed contiguous `count * 16` float array, or `NULL` for invalid,
///   empty, or unrecoverable state.
const float *rt_instbatch3d_borrow_transforms(void *batch, int32_t *out_count) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(batch, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (out_count)
        *out_count = 0;
    if (!b || !instbatch_repair_state(b) || !b->transforms || b->instance_count <= 0)
        return NULL;
    instbatch_sanitize_active_matrices(b);
    if (out_count)
        *out_count = b->instance_count;
    return b->transforms;
}

/// @brief Queue all visible instances for the active Canvas3D frame.
/// @details Validates batch resources, refreshes mesh bounds, converts
///   camera-relative double matrices when required, otherwise advances
///   once-per-frame motion history and conservatively frustum-culls individual
///   AABBs. Partial-cull scratch allocation failure falls back to the complete
///   batch, while representation/primary allocation failures report a trap.
///   Backend-native versus software fallback submission is selected downstream.
/// @param canvas_obj Canvas3D receiver or supported stack-wrapper handle with an active frame.
/// @param batch_obj InstanceBatch3D receiver borrowed for queue construction.
void rt_canvas3d_draw_instanced(void *canvas_obj, void *batch_obj) {
    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas_obj);
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(batch_obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    if (!c || !b)
        return;
    if (!instbatch_repair_state(b))
        return;
    if (!c->in_frame || !c->backend || b->instance_count == 0)
        return;
    if ((size_t)b->instance_count > SIZE_MAX / (16u * sizeof(float))) {
        rt_trap("InstanceBatch3D.Draw: instance matrix allocation overflow");
        return;
    }

    rt_mesh3d *mesh = (rt_mesh3d *)rt_g3d_checked_or_null(b->mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!mesh || mesh->vertex_count == 0 || mesh->index_count == 0)
        return;
    rt_mesh3d_refresh_bounds(mesh);

    float mesh_min[3] = {mesh->aabb_min[0], mesh->aabb_min[1], mesh->aabb_min[2]};
    float mesh_max[3] = {mesh->aabb_max[0], mesh->aabb_max[1], mesh->aabb_max[2]};
    vgfx3d_frustum_t frustum;
    vgfx3d_frustum_extract(&frustum, c->cached_vp);

    /* Build draw command from batch mesh/material */
    rt_material3d *mat =
        (rt_material3d *)rt_g3d_checked_or_null(b->material, RT_G3D_MATERIAL3D_CLASS_ID);
    if (!mat)
        return;
    if (canvas3d_uses_camera_relative_upload(c) && b->transforms64) {
        if (!instbatch_ensure_matrix_scratch(
                &b->visible_transforms, &b->visible_capacity, b->instance_count)) {
            rt_trap("InstanceBatch3D.Draw: camera-relative matrix scratch allocation failed");
            return;
        }
        for (int32_t i = 0; i < b->instance_count; i++) {
            const double *model = &b->transforms64[(size_t)i * 16u];
            if (!instbatch_matrix64_to_canvas_frame(
                    c, model, &b->visible_transforms[(size_t)i * 16u])) {
                rt_trap("InstanceBatch3D.Draw: camera-relative matrix is out of float range");
                return;
            }
        }
        rt_canvas3d_queue_instanced_batch_frame_matrices(
            canvas_obj, mesh, mat, b->visible_transforms, b->instance_count, NULL, 0);
        return;
    }
    instbatch_sanitize_active_matrices(b);

    if (b->last_motion_frame != rt_canvas3d_get_frame_serial(canvas_obj)) {
        if (b->motion_snapshot_count > 0) {
            memcpy(b->prev_transforms,
                   b->current_snapshot,
                   (size_t)b->motion_snapshot_count * 16 * sizeof(float));
            b->prev_count = b->motion_snapshot_count;
            b->has_prev_snapshot = 1;
        }
        memcpy(b->current_snapshot, b->transforms, (size_t)b->instance_count * 16u * sizeof(float));
        b->motion_snapshot_count = b->instance_count;
        b->last_motion_frame = rt_canvas3d_get_frame_serial(canvas_obj);
    }

    {
        const float *submit_transforms = b->transforms;
        const float *submit_prev = NULL;
        int8_t has_prev = 0;
        int32_t submit_count = b->instance_count;
        if (b->has_prev_snapshot && b->prev_count > 0) {
            if (b->prev_count == b->instance_count) {
                submit_prev = b->prev_transforms;
                has_prev = 1;
            } else {
                if (instbatch_ensure_matrix_scratch(
                        &b->prev_submit_transforms, &b->prev_submit_capacity, b->instance_count)) {
                    int32_t preserved =
                        b->prev_count < b->instance_count ? b->prev_count : b->instance_count;
                    if (preserved > 0) {
                        memcpy(b->prev_submit_transforms,
                               b->prev_transforms,
                               (size_t)preserved * 16u * sizeof(float));
                    }
                    if (preserved < b->instance_count) {
                        memcpy(&b->prev_submit_transforms[(size_t)preserved * 16u],
                               &b->transforms[(size_t)preserved * 16u],
                               (size_t)(b->instance_count - preserved) * 16u * sizeof(float));
                    }
                    submit_prev = b->prev_submit_transforms;
                    has_prev = 1;
                }
            }
        }
        if (mesh->bsphere_radius > 0.0f && b->instance_count > 0) {
            int32_t visible_count = 0;
            for (int32_t i = 0; i < b->instance_count; i++) {
                const float *src = &b->transforms[(size_t)i * 16u];
                if (instbatch_instance_visible(&frustum, mesh_min, mesh_max, src))
                    visible_count++;
            }
            if (visible_count == 0) {
                return;
            }
            if (visible_count < b->instance_count) {
                if (instbatch_ensure_matrix_scratch(
                        &b->visible_transforms, &b->visible_capacity, visible_count) &&
                    (!has_prev || instbatch_ensure_matrix_scratch(&b->visible_prev_transforms,
                                                                  &b->visible_prev_capacity,
                                                                  visible_count))) {
                    int32_t write_index = 0;
                    float *visible_prev = has_prev ? b->visible_prev_transforms : NULL;
                    for (int32_t i = 0; i < b->instance_count; i++) {
                        const float *src = &b->transforms[(size_t)i * 16u];
                        if (!instbatch_instance_visible(&frustum, mesh_min, mesh_max, src))
                            continue;
                        memcpy(&b->visible_transforms[(size_t)write_index * 16u],
                               src,
                               16u * sizeof(float));
                        if (visible_prev) {
                            memcpy(&visible_prev[(size_t)write_index * 16u],
                                   &submit_prev[(size_t)i * 16u],
                                   16u * sizeof(float));
                        }
                        write_index++;
                    }
                    submit_transforms = b->visible_transforms;
                    submit_prev = visible_prev;
                    submit_count = visible_count;
                    has_prev = visible_prev ? 1 : 0;
                }
            }
        }
        rt_canvas3d_queue_instanced_batch(
            canvas_obj, mesh, mat, submit_transforms, submit_count, submit_prev, has_prev);
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
