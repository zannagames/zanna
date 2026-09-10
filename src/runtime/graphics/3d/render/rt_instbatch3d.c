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
//   - Four primary buffers hold live doubles/floats and current/previous double history.
//   - Culling bounds are cached by mesh revision and prepared model matrix; views
//     always run their own frustum test and queued draws never borrow cache entries.
//
// Ownership/Lifetime:
//   - InstanceBatch3D is GC-managed; finalizer releases mesh, material,
//     the matrix buffers, and the retained culling-bounds cache.
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
///   per-instance frustum culling with revision-keyed bounds, and queue stable submissions.

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
/* ADR 0349 retained cells: the canvas split's spacing and per-axis cap, applied in 3D so a
 * parked layer far below a scene falls into its own cells. */
#define INSTBATCH_CELL_EXTENT 256.0f
#define INSTBATCH_CELL_AXIS_MAX 4
#define INSTBATCH_CELL_MAX (INSTBATCH_CELL_AXIS_MAX * INSTBATCH_CELL_AXIS_MAX * INSTBATCH_CELL_AXIS_MAX)
#define INSTBATCH_CELL_MIN_INSTANCES 64
#define INSTBATCH3D_FLOAT_ABS_MAX 3.40282346638528859812e38
#define INSTBATCH3D_WORLD_ABS_MAX 1000000000000.0

typedef struct {
    float matrix[16];
    float world_min[3];
    float world_max[3];
    uint64_t epoch;
    int8_t valid;
} instbatch_bounds_entry;

typedef struct {
    void *vptr;
    void *mesh;              /* retained Mesh3D */
    void *material;          /* retained Material3D */
    float *transforms;       /* N * 16 floats */
    float *current_snapshot; /* retired float-history mirror; always NULL */
    float *prev_transforms;  /* retired float-history mirror; always NULL */
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
    int32_t allocation_capacity; /* private bound for the four primary matrix buffers */
    double *current_snapshot64;
    double *prev_transforms64;
    uint8_t *visibility_mask;
    int32_t visibility_mask_capacity;
    int8_t motion_frame_initialized;
    /* Stable ownership identities for the four primary buffers. The retired float-history
     * pointers remain in-place for private-layout compatibility but are intentionally NULL. */
    float *owned_transforms;
    float *owned_current_snapshot;
    float *owned_prev_transforms;
    double *owned_transforms64;
    double *owned_current_snapshot64;
    double *owned_prev_transforms64;
    uint64_t bounds_refits; /* private diagnostic count of culling AABB calculations */
    instbatch_bounds_entry *bounds_cache;
    int32_t bounds_capacity;
    uint64_t bounds_epoch;
    const rt_mesh3d *bounds_mesh;
    uint32_t bounds_mesh_revision;
    float bounds_local_min[3];
    float bounds_local_max[3];
    float *submit_bounds;
    int32_t submit_bounds_capacity;
    /* ADR 0349: mutation tracking. `dirty_lo..dirty_hi` (inclusive; lo > hi is empty) is the
     * slot range Set touched since the last frame snapshot; `last_dirty_*` is the range the
     * previous frame's snapshot consumed. Structural changes (add/remove/clear/realloc/repair)
     * mark every slot. A stationary batch therefore sanitizes and snapshots nothing. */
    uint64_t revision;
    int32_t dirty_lo;
    int32_t dirty_hi;
    int8_t dirty_all;
    int32_t last_dirty_lo;
    int32_t last_dirty_hi;
    int8_t last_dirty_all;
    /* prev_submit_transforms mirrors prev_transforms64 (non-relative path) when set. */
    int8_t prev_submit_valid;
    /* ADR 0349 retained cells: bucket-contiguous member lists over a 3D grid, a
     * rotation-invariant bounding sphere per instance (centre = translation, radius =
     * (|local centre| + mesh radius) * max scale) and per-cell world AABBs. */
    int8_t cells_valid;
    int32_t cell_dims[3];
    float cell_origin[3];
    float cell_inv[3];
    int32_t cell_count;
    int32_t cell_start[INSTBATCH_CELL_MAX + 1];
    float cell_min[INSTBATCH_CELL_MAX][3];
    float cell_max[INSTBATCH_CELL_MAX][3];
    int8_t cell_bounded[INSTBATCH_CELL_MAX];
    int8_t cell_stale[INSTBATCH_CELL_MAX];
    int32_t *cell_members;
    int8_t *cell_of;
    float *cell_sphere;
    int32_t cell_capacity;
    const rt_mesh3d *cell_mesh;
    uint32_t cell_mesh_revision;
    float cell_local_radius;
} rt_instbatch3d;

/// @brief Mark every slot dirty after a structural change (ADR 0349).
static void instbatch_mark_dirty_all(rt_instbatch3d *b) {
    if (!b)
        return;
    b->dirty_all = 1;
    b->dirty_lo = 0;
    b->dirty_hi = -1;
    b->prev_submit_valid = 0;
    b->revision++;
}

/// @brief Widen the dirty slot range with one mutated slot (ADR 0349).
static void instbatch_mark_dirty(rt_instbatch3d *b, int32_t index) {
    if (!b || index < 0)
        return;
    if (b->dirty_lo > b->dirty_hi) {
        b->dirty_lo = index;
        b->dirty_hi = index;
    } else {
        if (index < b->dirty_lo)
            b->dirty_lo = index;
        if (index > b->dirty_hi)
            b->dirty_hi = index;
    }
    b->revision++;
}

/// @brief Clamp a dirty range to the live slots; returns 0 for an empty range.
static int instbatch_dirty_span(int32_t lo, int32_t hi, int32_t count, int32_t *out_lo,
                                int32_t *out_hi) {
    if (lo < 0)
        lo = 0;
    if (hi > count - 1)
        hi = count - 1;
    if (lo > hi)
        return 0;
    *out_lo = lo;
    *out_hi = hi;
    return 1;
}

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

/// @brief Replace invalid/extreme doubles in one authoritative matrix slot.
/// @param slot In/out array of sixteen double matrix elements; `NULL` is ignored.
static void instbatch_sanitize_matrix_slot64(double *slot) {
    if (!slot)
        return;
    for (int i = 0; i < 16; i++)
        slot[i] = instbatch_sanitize_matrix_value64(slot[i], i / 4, i % 4);
}

/// @brief Repair all active matrix slots before motion snapshots or backend submission.
/// @details Regenerates live float mirrors from authoritative doubles and
///   sanitizes the double-precision current/previous motion snapshots in place.
/// @param b Batch whose active matrix ranges are repaired; `NULL` is ignored.
static void instbatch_sanitize_active_matrices(rt_instbatch3d *b) {
    int32_t lo;
    int32_t hi;
    if (!b)
        return;
    /* ADR 0349: slots outside the dirty range were sanitized when they were last written
     * and have not changed since; a stationary batch does no work here. */
    if (b->dirty_all) {
        lo = 0;
        hi = b->instance_count - 1;
    } else if (!instbatch_dirty_span(b->dirty_lo, b->dirty_hi, b->instance_count, &lo, &hi)) {
        return;
    }
    for (int32_t i = lo; i <= hi; i++) {
        if (b->transforms64)
            instbatch_copy_matrix64_to_float(&b->transforms[(size_t)i * 16u],
                                             &b->transforms64[(size_t)i * 16u]);
        else
            instbatch_sanitize_matrix_slot(&b->transforms[(size_t)i * 16u]);
    }
    for (int32_t i = lo; i <= hi && i < b->motion_snapshot_count; i++)
        instbatch_sanitize_matrix_slot64(&b->current_snapshot64[(size_t)i * 16u]);
    for (int32_t i = lo; i <= hi && i < b->prev_count; i++)
        instbatch_sanitize_matrix_slot64(&b->prev_transforms64[(size_t)i * 16u]);
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

/// @brief Ensure the reusable one-byte-per-instance visibility mask can hold @p needed entries.
/// @param b Batch owning the scratch mask.
/// @param needed Required instance capacity.
/// @return Nonzero when the mask is available; zero on invalid input, overflow, or allocation
/// failure.
static int instbatch_ensure_visibility_mask(rt_instbatch3d *b, int32_t needed) {
    uint8_t *grown;
    int32_t capacity;
    if (!b || needed < 0)
        return 0;
    if (needed == 0)
        return 1;
    if (b->visibility_mask && b->visibility_mask_capacity >= needed)
        return 1;
    capacity = b->visibility_mask_capacity > 0 ? b->visibility_mask_capacity : INST_INIT_CAP;
    while (capacity < needed) {
        if (capacity > INT32_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    grown = (uint8_t *)realloc(b->visibility_mask, (size_t)capacity);
    if (!grown)
        return 0;
    b->visibility_mask = grown;
    b->visibility_mask_capacity = capacity;
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
/// @details Uses private ownership identities and allocation capacity before traversing any
///   mutable count, restores damaged legacy pointers, and publishes a fresh empty primary set only
///   after every replacement allocation succeeds. Counts and flags are normalized against the
///   real allocation bound.
/// @param b Batch to repair in place.
/// @return Nonzero when four coherent primary matrix buffers are available;
///   zero for null input or reconstruction/allocation failure.
static int instbatch_repair_state(rt_instbatch3d *b) {
    if (!b)
        return 0;
    instbatch_repair_resource_handles(b);
    if (b->allocation_capacity <= 0 ||
        (size_t)b->allocation_capacity > SIZE_MAX / (16u * sizeof(double)) ||
        !b->owned_transforms || !b->owned_transforms64 || !b->owned_current_snapshot64 ||
        !b->owned_prev_transforms64) {
        double *new_transforms64 = (double *)calloc(INST_INIT_CAP * 16u, sizeof(double));
        double *new_current64 = (double *)calloc(INST_INIT_CAP * 16u, sizeof(double));
        double *new_prev64 = (double *)calloc(INST_INIT_CAP * 16u, sizeof(double));
        float *new_transforms = (float *)calloc(INST_INIT_CAP * 16u, sizeof(float));
        if (!new_transforms64 || !new_current64 || !new_prev64 || !new_transforms) {
            free(new_transforms64);
            free(new_current64);
            free(new_prev64);
            free(new_transforms);
            return 0;
        }
        free(b->owned_transforms64);
        free(b->owned_current_snapshot64);
        free(b->owned_prev_transforms64);
        free(b->owned_transforms);
        free(b->owned_current_snapshot);
        free(b->owned_prev_transforms);
        b->owned_transforms64 = new_transforms64;
        b->owned_current_snapshot64 = new_current64;
        b->owned_prev_transforms64 = new_prev64;
        b->owned_transforms = new_transforms;
        b->owned_current_snapshot = NULL;
        b->owned_prev_transforms = NULL;
        b->transforms64 = new_transforms64;
        b->current_snapshot64 = new_current64;
        b->prev_transforms64 = new_prev64;
        b->transforms = new_transforms;
        b->current_snapshot = NULL;
        b->prev_transforms = NULL;
        b->instance_count = 0;
        b->motion_snapshot_count = 0;
        b->prev_count = 0;
        b->has_prev_snapshot = 0;
        b->motion_frame_initialized = 0;
        b->instance_capacity = INST_INIT_CAP;
        b->allocation_capacity = INST_INIT_CAP;
        instbatch_mark_dirty_all(b);
        return 1;
    }
    b->transforms = b->owned_transforms;
    b->current_snapshot = NULL;
    b->prev_transforms = NULL;
    b->transforms64 = b->owned_transforms64;
    b->current_snapshot64 = b->owned_current_snapshot64;
    b->prev_transforms64 = b->owned_prev_transforms64;
    b->instance_capacity = b->allocation_capacity;
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
    b->motion_frame_initialized = b->motion_frame_initialized ? 1 : 0;
    if (!b->visible_transforms)
        b->visible_capacity = 0;
    if (!b->visible_prev_transforms)
        b->visible_prev_capacity = 0;
    if (!b->prev_submit_transforms)
        b->prev_submit_capacity = 0;
    if (!b->visibility_mask)
        b->visibility_mask_capacity = 0;
    return 1;
}

/// @brief Prepare retained culling bounds for a mesh revision and bounded instance capacity.
/// @details Allocation failure leaves the uncached path available; no queued draw borrows this
///   storage. Local bounds are part of the key to cover defensive mesh-bound repairs too.
/// @param b Owned batch; cache allocation never exceeds geometric growth for its live count.
/// @param mesh Borrowed validated mesh whose local bounds have been refreshed.
static void instbatch_prepare_bounds_cache(rt_instbatch3d *b, const rt_mesh3d *mesh) {
    if (b->bounds_epoch == 0 || b->bounds_mesh != mesh ||
        b->bounds_mesh_revision != mesh->geometry_revision ||
        memcmp(b->bounds_local_min, mesh->aabb_min, sizeof(b->bounds_local_min)) != 0 ||
        memcmp(b->bounds_local_max, mesh->aabb_max, sizeof(b->bounds_local_max)) != 0) {
        if (b->bounds_epoch == UINT64_MAX) {
            if (b->bounds_cache && b->bounds_capacity > 0)
                memset(b->bounds_cache, 0, (size_t)b->bounds_capacity * sizeof(*b->bounds_cache));
            b->bounds_epoch = 1;
        } else {
            b->bounds_epoch++;
        }
        b->bounds_mesh = mesh;
        b->bounds_mesh_revision = mesh->geometry_revision;
        memcpy(b->bounds_local_min, mesh->aabb_min, sizeof(b->bounds_local_min));
        memcpy(b->bounds_local_max, mesh->aabb_max, sizeof(b->bounds_local_max));
    }
    if (b->bounds_capacity < b->instance_count) {
        int32_t capacity = b->bounds_capacity;
        while (capacity < b->instance_count) {
            if (!instbatch_next_capacity(capacity, &capacity) ||
                (size_t)capacity > SIZE_MAX / sizeof(instbatch_bounds_entry))
                return;
        }
        instbatch_bounds_entry *entries =
            (instbatch_bounds_entry *)calloc((size_t)capacity, sizeof(*entries));
        if (!entries)
            return;
        if (b->bounds_cache && b->bounds_capacity > 0)
            memcpy(entries, b->bounds_cache, (size_t)b->bounds_capacity * sizeof(*entries));
        free(b->bounds_cache);
        b->bounds_cache = entries;
        b->bounds_capacity = capacity;
    }
}

/// @brief Reserve packed visible bounds; allocation failure keeps ordinary submission available.
/// @param b Batch owning six floats per scratch instance.
static int instbatch_prepare_submit_bounds(rt_instbatch3d *b) {
    if (b->bounds_capacity < b->instance_count || !b->bounds_cache)
        return 0;
    if (b->submit_bounds_capacity >= b->instance_count)
        return 1;
    int32_t capacity = b->submit_bounds_capacity;
    while (capacity < b->instance_count) {
        if (!instbatch_next_capacity(capacity, &capacity) ||
            (size_t)capacity > SIZE_MAX / (6u * sizeof(float)))
            return 0;
    }
    float *bounds = (float *)realloc(b->submit_bounds, (size_t)capacity * 6u * sizeof(float));
    if (!bounds)
        return 0;
    b->submit_bounds = bounds;
    b->submit_bounds_capacity = capacity;
    return 1;
}

/// @brief Pack an already-validated culling entry for the corresponding visible instance.
/// @param b Batch whose culling lookup has just prepared the source entry.
/// @param source Original instance index.
/// @param target Compacted visible index in scratch storage.
static int instbatch_copy_submit_bounds(rt_instbatch3d *b, int32_t source, int32_t target) {
    const instbatch_bounds_entry *entry = &b->bounds_cache[source];
    if (entry->epoch != b->bounds_epoch || !entry->valid)
        return 0;
    memcpy(b->submit_bounds + (size_t)target * 6u, entry->world_min, 3u * sizeof(float));
    memcpy(b->submit_bounds + (size_t)target * 6u + 3u, entry->world_max, 3u * sizeof(float));
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
/// @param b Borrowed batch owning optional retained culling bounds.
/// @param index Original instance slot, independent of visibility compaction.
/// @param frustum Borrowed camera frustum; `NULL` disables rejection.
/// @param mesh_min Three-element local-space minimum AABB corner.
/// @param mesh_max Three-element local-space maximum AABB corner.
/// @param model_matrix Sixteen-element row-major instance transform.
/// @return 1 if the instance's world AABB is on-screen or intersecting, 0 if
///   definitively outside the frustum.
static int instbatch_instance_visible(rt_instbatch3d *b,
                                      int32_t index,
                                      const vgfx3d_frustum_t *frustum,
                                      const float mesh_min[3],
                                      const float mesh_max[3],
                                      const float *model_matrix) {
    double world_matrix[16];
    float world_min[3];
    float world_max[3];

    if (!frustum || !mesh_min || !mesh_max || !model_matrix)
        return 1;

    instbatch_bounds_entry *entry = b->bounds_cache && index >= 0 && index < b->bounds_capacity
                                        ? &b->bounds_cache[index]
                                        : NULL;
    if (entry && entry->epoch == b->bounds_epoch && b->bounds_epoch != 0 &&
        memcmp(entry->matrix, model_matrix, sizeof(entry->matrix)) == 0) {
        return !entry->valid ||
               vgfx3d_frustum_test_aabb(frustum, entry->world_min, entry->world_max) != 0;
    }
    for (int i = 0; i < 16; i++)
        world_matrix[i] = (double)model_matrix[i];
    int valid =
        vgfx3d_transform_aabb_checked(mesh_min, mesh_max, world_matrix, world_min, world_max);
    if (b->bounds_refits < UINT64_MAX)
        b->bounds_refits++;
    if (entry) {
        memcpy(entry->matrix, model_matrix, sizeof(entry->matrix));
        entry->epoch = b->bounds_epoch;
        entry->valid = valid ? 1 : 0;
        if (valid) {
            memcpy(entry->world_min, world_min, sizeof(entry->world_min));
            memcpy(entry->world_max, world_max, sizeof(entry->world_max));
        }
    }
    return !valid || vgfx3d_frustum_test_aabb(frustum, world_min, world_max) != 0;
}

/// @brief Grow the retained cell storage to hold @p needed instances (ADR 0349).
static int instbatch_cells_reserve(rt_instbatch3d *b, int32_t needed) {
    int32_t capacity;
    int32_t *members;
    int8_t *cell_of;
    float *spheres;
    if (!b || needed <= 0)
        return 0;
    if (b->cell_members && b->cell_of && b->cell_sphere && b->cell_capacity >= needed)
        return 1;
    capacity = b->cell_capacity > 0 ? b->cell_capacity : INST_INIT_CAP;
    while (capacity < needed) {
        if (!instbatch_next_capacity(capacity, &capacity))
            return 0;
    }
    if ((size_t)capacity > SIZE_MAX / (4u * sizeof(float)))
        return 0;
    members = (int32_t *)realloc(b->cell_members, (size_t)capacity * sizeof(int32_t));
    if (!members)
        return 0;
    b->cell_members = members;
    cell_of = (int8_t *)realloc(b->cell_of, (size_t)capacity);
    if (!cell_of)
        return 0;
    b->cell_of = cell_of;
    spheres = (float *)realloc(b->cell_sphere, (size_t)capacity * 4u * sizeof(float));
    if (!spheres)
        return 0;
    b->cell_sphere = spheres;
    b->cell_capacity = capacity;
    return 1;
}

/// @brief Rotation-invariant bounding sphere of one instance from its float matrix.
/// @details Centre is the translation; radius scales the local radius by the largest column
///   length, so a yaw-only change leaves the sphere untouched. Non-finite input gives a
///   negative radius, which keeps its cell conservatively unbounded.
static void instbatch_instance_sphere(const float *m, float local_radius, float out[4]) {
    float sx = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    float sy = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    float sz = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
    float scale = sx > sy ? sx : sy;
    if (sz > scale)
        scale = sz;
    out[0] = m[3];
    out[1] = m[7];
    out[2] = m[11];
    out[3] = local_radius * scale;
    if (!isfinite(out[0]) || !isfinite(out[1]) || !isfinite(out[2]) || !isfinite(out[3]) ||
        out[3] < 0.0f)
        out[3] = -1.0f;
}

/// @brief Grid cell of a sphere centre, clamped to the retained grid.
static int32_t instbatch_cell_index_for(const rt_instbatch3d *b, const float *sphere) {
    int32_t idx[3];
    for (int axis = 0; axis < 3; axis++) {
        float v = (sphere[axis] - b->cell_origin[axis]) * b->cell_inv[axis];
        int32_t c = isfinite(v) ? (int32_t)v : 0;
        if (c < 0)
            c = 0;
        if (c >= b->cell_dims[axis])
            c = b->cell_dims[axis] - 1;
        idx[axis] = c;
    }
    return (idx[2] * b->cell_dims[1] + idx[1]) * b->cell_dims[0] + idx[0];
}

/// @brief Recompute one cell's world AABB from its members' spheres.
static void instbatch_cell_refit(rt_instbatch3d *b, int32_t cell) {
    int bounded = 0;
    int32_t begin = b->cell_start[cell];
    int32_t end = b->cell_start[cell + 1];
    b->cell_bounded[cell] = 0;
    for (int32_t k = begin; k < end; k++) {
        const float *sp = &b->cell_sphere[(size_t)b->cell_members[k] * 4u];
        if (sp[3] < 0.0f) {
            b->cell_bounded[cell] = 0;
            b->cell_stale[cell] = 0;
            return; /* an invalid member keeps the cell conservatively unbounded */
        }
        for (int axis = 0; axis < 3; axis++) {
            float lo = sp[axis] - sp[3];
            float hi = sp[axis] + sp[3];
            if (!bounded || lo < b->cell_min[cell][axis])
                b->cell_min[cell][axis] = lo;
            if (!bounded || hi > b->cell_max[cell][axis])
                b->cell_max[cell][axis] = hi;
        }
        bounded = 1;
    }
    b->cell_bounded[cell] = (int8_t)bounded;
    b->cell_stale[cell] = 0;
}

/// @brief Rebuild the grid, membership lists and cell bounds from every instance.
static int instbatch_cells_rebuild(rt_instbatch3d *b) {
    float lo[3] = {0.0f, 0.0f, 0.0f};
    float hi[3] = {0.0f, 0.0f, 0.0f};
    int have = 0;
    int32_t cursor[INSTBATCH_CELL_MAX];
    for (int32_t i = 0; i < b->instance_count; i++) {
        float *sp = &b->cell_sphere[(size_t)i * 4u];
        instbatch_instance_sphere(&b->transforms[(size_t)i * 16u], b->cell_local_radius, sp);
        if (sp[3] < 0.0f)
            continue;
        for (int axis = 0; axis < 3; axis++) {
            if (!have || sp[axis] < lo[axis])
                lo[axis] = sp[axis];
            if (!have || sp[axis] > hi[axis])
                hi[axis] = sp[axis];
        }
        have = 1;
    }
    b->cell_count = 1;
    for (int axis = 0; axis < 3; axis++) {
        float extent = have ? hi[axis] - lo[axis] : 0.0f;
        int32_t dims = 1;
        if (isfinite(extent) && extent > INSTBATCH_CELL_EXTENT) {
            dims = (int32_t)(extent / INSTBATCH_CELL_EXTENT) + 1;
            if (dims > INSTBATCH_CELL_AXIS_MAX)
                dims = INSTBATCH_CELL_AXIS_MAX;
        }
        b->cell_dims[axis] = dims;
        b->cell_origin[axis] = lo[axis];
        b->cell_inv[axis] =
            dims > 1 && extent > 1e-6f ? (float)dims / (extent * 1.0001f) : 0.0f;
        b->cell_count *= dims;
    }
    memset(b->cell_start, 0, sizeof(b->cell_start));
    for (int32_t i = 0; i < b->instance_count; i++) {
        int32_t cell = instbatch_cell_index_for(b, &b->cell_sphere[(size_t)i * 4u]);
        b->cell_of[i] = (int8_t)cell;
        b->cell_start[cell + 1]++;
    }
    for (int32_t cell = 0; cell < b->cell_count; cell++)
        b->cell_start[cell + 1] += b->cell_start[cell];
    memcpy(cursor, b->cell_start, sizeof(int32_t) * (size_t)b->cell_count);
    for (int32_t i = 0; i < b->instance_count; i++)
        b->cell_members[cursor[b->cell_of[i]]++] = i;
    for (int32_t cell = 0; cell < b->cell_count; cell++)
        instbatch_cell_refit(b, cell);
    b->cells_valid = 1;
    return 1;
}

/// @brief Bring the retained cells up to date with the batch's dirty range (ADR 0349).
/// @return 1 when the cells may drive culling and submission this draw, 0 for the
///   per-instance path (small batch, allocation failure, or a stale grid).
static int instbatch_cells_refresh(rt_instbatch3d *b, const rt_mesh3d *mesh) {
    float centre[3];
    float local_radius;
    int32_t lo;
    int32_t hi;
    if (!b || !mesh || b->instance_count < INSTBATCH_CELL_MIN_INSTANCES) {
        if (b)
            b->cells_valid = 0;
        return 0;
    }
    if (!instbatch_cells_reserve(b, b->instance_count)) {
        b->cells_valid = 0;
        return 0;
    }
    for (int axis = 0; axis < 3; axis++)
        centre[axis] = 0.5f * (mesh->aabb_min[axis] + mesh->aabb_max[axis]);
    local_radius = sqrtf(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]) +
                   mesh->bsphere_radius;
    if (!isfinite(local_radius) || local_radius < 0.0f)
        local_radius = 0.0f;
    if (!b->cells_valid || b->dirty_all || b->cell_mesh != mesh ||
        b->cell_mesh_revision != mesh->geometry_revision ||
        b->cell_local_radius != local_radius) {
        b->cell_mesh = mesh;
        b->cell_mesh_revision = mesh->geometry_revision;
        b->cell_local_radius = local_radius;
        return instbatch_cells_rebuild(b);
    }
    if (instbatch_dirty_span(b->dirty_lo, b->dirty_hi, b->instance_count, &lo, &hi)) {
        for (int32_t i = lo; i <= hi; i++) {
            float sphere[4];
            float *stored = &b->cell_sphere[(size_t)i * 4u];
            instbatch_instance_sphere(&b->transforms[(size_t)i * 16u], local_radius, sphere);
            if (memcmp(sphere, stored, sizeof(sphere)) == 0)
                continue; /* a rotation-only change: same sphere, same cell */
            if (instbatch_cell_index_for(b, sphere) != b->cell_of[i])
                return instbatch_cells_rebuild(b);
            memcpy(stored, sphere, sizeof(sphere));
            b->cell_stale[b->cell_of[i]] = 1;
        }
        for (int32_t cell = 0; cell < b->cell_count; cell++) {
            if (b->cell_stale[cell])
                instbatch_cell_refit(b, cell);
        }
    }
    return 1;
}

/// @brief Classify a cell AABB against the frustum: 0 outside, 1 partial, 2 inside.
static int instbatch_cell_classify(const vgfx3d_frustum_t *f, const float mn[3], const float mx[3]) {
    int inside = 1;
    if (!f || !f->planes_valid)
        return 1;
    for (int p = 0; p < 6; p++) {
        const float *pl = f->planes[p];
        float pv = 0.0f;
        float nv = 0.0f;
        for (int axis = 0; axis < 3; axis++) {
            float a = pl[axis];
            pv += a * (a >= 0.0f ? mx[axis] : mn[axis]);
            nv += a * (a >= 0.0f ? mn[axis] : mx[axis]);
        }
        if (pv + pl[3] < 0.0f)
            return 0;
        if (nv + pl[3] < 0.0f)
            inside = 0;
    }
    return inside ? 2 : 1;
}

/// @brief GC finalizer — release the current-frame and motion-history buffers.
/// @details Instance batches keep three authoritative double-matrix arrays for live,
///   start-of-frame, and previous-frame state, plus one float submit mirror for live state.
///   All four are plain heap allocations with no downstream refs to release. Counters are zeroed
///   post-free so a lingering post-finalize read sees an empty batch rather than
///   capacity-matches-missing-buffer.
/// @param obj InstanceBatch3D payload being finalized; `NULL` is ignored.
static void instbatch_finalizer(void *obj) {
    rt_instbatch3d *b = (rt_instbatch3d *)obj;
    if (!b)
        return;
    free(b->cell_members);
    free(b->cell_of);
    free(b->cell_sphere);
    b->cell_members = NULL;
    b->cell_of = NULL;
    b->cell_sphere = NULL;
    b->cell_capacity = 0;
    b->cells_valid = 0;
    free(b->owned_transforms);
    free(b->owned_transforms64);
    free(b->owned_current_snapshot);
    free(b->owned_prev_transforms);
    free(b->owned_current_snapshot64);
    free(b->owned_prev_transforms64);
    free(b->visible_transforms);
    free(b->visible_prev_transforms);
    free(b->prev_submit_transforms);
    free(b->visibility_mask);
    free(b->bounds_cache);
    free(b->submit_bounds);
    b->submit_bounds = NULL;
    b->submit_bounds_capacity = 0;
    b->bounds_cache = NULL;
    b->bounds_capacity = 0;
    b->bounds_epoch = 0;
    b->bounds_mesh = NULL;
    b->transforms = NULL;
    b->transforms64 = NULL;
    b->current_snapshot = NULL;
    b->prev_transforms = NULL;
    b->current_snapshot64 = NULL;
    b->prev_transforms64 = NULL;
    b->visible_transforms = NULL;
    b->visible_prev_transforms = NULL;
    b->prev_submit_transforms = NULL;
    b->visibility_mask = NULL;
    b->owned_transforms = NULL;
    b->owned_transforms64 = NULL;
    b->owned_current_snapshot = NULL;
    b->owned_prev_transforms = NULL;
    b->owned_current_snapshot64 = NULL;
    b->owned_prev_transforms64 = NULL;
    b->instance_count = b->instance_capacity = 0;
    b->allocation_capacity = 0;
    b->visible_capacity = b->visible_prev_capacity = b->prev_submit_capacity = 0;
    b->motion_snapshot_count = b->prev_count = 0;
    b->visibility_mask_capacity = 0;
    b->last_motion_frame = 0;
    b->has_prev_snapshot = 0;
    b->motion_frame_initialized = 0;
    instbatch_release_mesh_slot(&b->mesh);
    instbatch_release_material_slot(&b->material);
}

/// @brief Create an instance batch for drawing N copies of one mesh efficiently.
/// @details Instance batching draws many objects with the same mesh and material
///          but different transforms in fewer draw calls. The software backend
///          falls back to individual draws; GPU backends may use native instancing.
///          Transforms are stored as contiguous double[16*N] row-major Mat4 arrays. A float
///          mirror is retained only for live backend submission; motion history is narrowed into
///          reusable draw scratch on demand.
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
    b->owned_transforms64 = (double *)calloc(INST_INIT_CAP * 16, sizeof(double));
    b->owned_transforms = (float *)calloc(INST_INIT_CAP * 16, sizeof(float));
    b->owned_current_snapshot = NULL;
    b->owned_prev_transforms = NULL;
    b->owned_current_snapshot64 = (double *)calloc(INST_INIT_CAP * 16, sizeof(double));
    b->owned_prev_transforms64 = (double *)calloc(INST_INIT_CAP * 16, sizeof(double));
    b->transforms64 = b->owned_transforms64;
    b->transforms = b->owned_transforms;
    b->current_snapshot = b->owned_current_snapshot;
    b->prev_transforms = b->owned_prev_transforms;
    b->current_snapshot64 = b->owned_current_snapshot64;
    b->prev_transforms64 = b->owned_prev_transforms64;
    b->instance_count = 0;
    b->instance_capacity = INST_INIT_CAP;
    b->allocation_capacity = INST_INIT_CAP;
    b->motion_snapshot_count = 0;
    b->prev_count = 0;
    b->last_motion_frame = 0;
    b->has_prev_snapshot = 0;
    b->motion_frame_initialized = 0;
    b->revision = 0;
    b->dirty_lo = 0;
    b->dirty_hi = -1;
    b->dirty_all = 1;
    b->last_dirty_lo = 0;
    b->last_dirty_hi = -1;
    b->last_dirty_all = 1;
    b->prev_submit_valid = 0;
    if (!b->owned_transforms64 || !b->owned_transforms || !b->owned_current_snapshot64 ||
        !b->owned_prev_transforms64) {
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
///   its float submit mirror. All four primary arrays grow transactionally and
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
        double *new_current64 = (double *)malloc(new_bytes64);
        double *new_prev64 = (double *)malloc(new_bytes64);
        float *new_transforms = (float *)malloc(new_bytes);
        if (!new_transforms64 || !new_current64 || !new_prev64 || !new_transforms) {
            free(new_transforms64);
            free(new_current64);
            free(new_prev64);
            free(new_transforms);
            rt_trap("InstanceBatch3D.Add: allocation failed");
            return;
        }
        if (old_bytes > 0) {
            memcpy(new_transforms64, b->transforms64, old_bytes64);
            memcpy(new_current64, b->current_snapshot64, old_bytes64);
            memcpy(new_prev64, b->prev_transforms64, old_bytes64);
            memcpy(new_transforms, b->transforms, old_bytes);
        }
        free(b->owned_transforms64);
        free(b->owned_current_snapshot64);
        free(b->owned_prev_transforms64);
        free(b->owned_transforms);
        free(b->owned_current_snapshot);
        free(b->owned_prev_transforms);
        b->owned_transforms64 = new_transforms64;
        b->owned_current_snapshot64 = new_current64;
        b->owned_prev_transforms64 = new_prev64;
        b->owned_transforms = new_transforms;
        b->owned_current_snapshot = NULL;
        b->owned_prev_transforms = NULL;
        b->transforms64 = new_transforms64;
        b->current_snapshot64 = new_current64;
        b->prev_transforms64 = new_prev64;
        b->transforms = new_transforms;
        b->current_snapshot = NULL;
        b->prev_transforms = NULL;
        b->instance_capacity = new_cap;
        b->allocation_capacity = new_cap;
    }

    double *dst64 = &b->transforms64[(size_t)b->instance_count * 16u];
    float *dst = &b->transforms[(size_t)b->instance_count * 16u];
    instbatch_copy_mat4_sanitized64(dst64, transform);
    instbatch_copy_matrix64_to_float(dst, dst64);

    b->instance_count++;
    instbatch_mark_dirty_all(b);
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
        if (last_idx >= b->motion_snapshot_count && index < b->motion_snapshot_count)
            b->motion_snapshot_count = (int32_t)index;
        if (last_idx >= b->prev_count && index < b->prev_count) {
            b->prev_count = (int32_t)index;
            b->has_prev_snapshot = b->prev_count > 0 ? 1 : 0;
        }
        if (last_idx < b->motion_snapshot_count) {
            memcpy(&b->current_snapshot64[(size_t)index * 16u],
                   &b->current_snapshot64[(size_t)last_idx * 16u],
                   16u * sizeof(double));
        }
        if (last_idx < b->prev_count) {
            memcpy(&b->prev_transforms64[(size_t)index * 16u],
                   &b->prev_transforms64[(size_t)last_idx * 16u],
                   16u * sizeof(double));
        }
    }
    b->instance_count--;
    if (b->motion_snapshot_count > b->instance_count)
        b->motion_snapshot_count = b->instance_count;
    if (b->prev_count > b->instance_count)
        b->prev_count = b->instance_count;
    instbatch_mark_dirty_all(b);
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
    instbatch_mark_dirty(b, (int32_t)index);
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
    b->motion_frame_initialized = 0;
    instbatch_mark_dirty_all(b);
}

/// @brief Retained CPU storage owned by the batch (ADR 0349): matrices, snapshots, mirrors,
///   scratch, culling bounds and the object itself. Not GPU memory.
/// @param obj InstanceBatch3D receiver; invalid handles report zero.
/// @return Bytes currently allocated for the batch.
int64_t rt_instbatch3d_retained_bytes(void *obj) {
    rt_instbatch3d *b =
        (rt_instbatch3d *)rt_g3d_checked_or_null(obj, RT_G3D_INSTANCEBATCH3D_CLASS_ID);
    uint64_t bytes;
    if (!b || !instbatch_repair_state(b))
        return 0;
    bytes = sizeof(*b);
    bytes += (uint64_t)b->allocation_capacity * 16u * (sizeof(float) + 3u * sizeof(double));
    bytes += (uint64_t)(b->visible_capacity > 0 ? b->visible_capacity : 0) * 16u * sizeof(float);
    bytes += (uint64_t)(b->visible_prev_capacity > 0 ? b->visible_prev_capacity : 0) * 16u *
             sizeof(float);
    bytes += (uint64_t)(b->prev_submit_capacity > 0 ? b->prev_submit_capacity : 0) * 16u *
             sizeof(float);
    bytes += (uint64_t)(b->visibility_mask_capacity > 0 ? b->visibility_mask_capacity : 0);
    bytes += (uint64_t)(b->bounds_capacity > 0 ? b->bounds_capacity : 0) *
             sizeof(instbatch_bounds_entry);
    bytes += (uint64_t)(b->submit_bounds_capacity > 0 ? b->submit_bounds_capacity : 0) * 6u *
             sizeof(float);
    bytes += (uint64_t)(b->cell_capacity > 0 ? b->cell_capacity : 0) *
             (sizeof(int32_t) + sizeof(int8_t) + 4u * sizeof(float));
    return bytes > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)bytes;
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
/// @brief Cull, compact and queue one segment of instances (a retained cell, or the whole
///   batch when cells are off), in world or camera-relative frame space (ADR 0349).
/// @param members Original indices in submission order, or `NULL` for 0..count-1.
/// @param test_each Non-zero to frustum-test members individually (a partial cell); an
///   inside cell still refreshes each member's cached bounds so prepared bounds stay exact.
static void instbatch_queue_segment(void *canvas_obj,
                                    rt_canvas3d *c,
                                    rt_instbatch3d *b,
                                    rt_mesh3d *mesh,
                                    rt_material3d *mat,
                                    const int32_t *members,
                                    int32_t count,
                                    const vgfx3d_frustum_t *frustum,
                                    const float mesh_min[3],
                                    const float mesh_max[3],
                                    int8_t has_prev,
                                    const float *prev_floats,
                                    int camera_relative,
                                    int test_each) {
    int32_t visible = 0;
    int prepared = frustum != NULL && mesh->bsphere_radius > 0.0f && instbatch_prepare_submit_bounds(b);
    for (int32_t k = 0; k < count; k++) {
        int32_t i = members ? members[k] : k;
        int vis;
        if (i < 0 || i >= b->instance_count)
            continue;
        if (camera_relative) {
            const double *model = &b->transforms64[(size_t)i * 16u];
            float current[16];
            if (!instbatch_matrix64_to_canvas_frame(c, model, current)) {
                rt_trap("InstanceBatch3D.Draw: camera-relative matrix is out of float range");
                return;
            }
            /* Inside cells refresh the cache with a null frustum (always visible). */
            vis = mesh->bsphere_radius > 0.0f
                      ? instbatch_instance_visible(b, i, frustum, mesh_min, mesh_max, current)
                      : 1;
            if (!test_each && frustum && mesh->bsphere_radius > 0.0f)
                vis = 1;
            if (b->visibility_mask && i < b->visibility_mask_capacity)
                b->visibility_mask[i] = vis ? 1u : 0u;
            if (!vis)
                continue;
            if (prepared && !instbatch_copy_submit_bounds(b, i, visible))
                prepared = 0;
            memcpy(&b->visible_transforms[(size_t)visible * 16u], current, sizeof(current));
            if (has_prev) {
                const double *previous =
                    i < b->prev_count ? &b->prev_transforms64[(size_t)i * 16u] : model;
                if (!instbatch_matrix64_to_canvas_frame(
                        c, previous, &b->prev_submit_transforms[(size_t)visible * 16u])) {
                    rt_trap("InstanceBatch3D.Draw: previous camera-relative matrix is out of float "
                            "range");
                    return;
                }
            }
        } else {
            const float *src = &b->transforms[(size_t)i * 16u];
            vis = mesh->bsphere_radius > 0.0f
                      ? instbatch_instance_visible(b, i, frustum, mesh_min, mesh_max, src)
                      : 1;
            if (!test_each && frustum && mesh->bsphere_radius > 0.0f)
                vis = 1;
            b->visibility_mask[i] = vis ? 1u : 0u;
            if (!vis)
                continue;
            if (prepared && !instbatch_copy_submit_bounds(b, i, visible))
                prepared = 0;
            memcpy(&b->visible_transforms[(size_t)visible * 16u], src, 16u * sizeof(float));
            if (has_prev)
                memcpy(&b->visible_prev_transforms[(size_t)visible * 16u],
                       &prev_floats[(size_t)i * 16u],
                       16u * sizeof(float));
        }
        visible++;
    }
    if (visible == 0)
        return;
    rt_canvas3d_queue_instanced_batch_prepared(
        canvas_obj,
        mesh,
        mat,
        b->visible_transforms,
        visible,
        has_prev ? (camera_relative ? b->prev_submit_transforms : b->visible_prev_transforms) : NULL,
        has_prev,
        prepared ? b->submit_bounds : NULL,
        camera_relative ? 1 : 0);
}

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
    // Camera-invisible instances can still shadow visible receivers. Until
    // main/shadow instance lists are separate, preserve potential casters and
    // let the light-volume tests reject whole batches conservatively.
    const vgfx3d_frustum_t *instance_frustum =
        c->shadows_enabled && !c->frame_is_view_model &&
                mat->shadow_mode != RT_MATERIAL3D_SHADOW_MODE_NONE
            ? NULL
            : &frustum;
    if (instance_frustum)
        instbatch_prepare_bounds_cache(b, mesh);
    instbatch_sanitize_active_matrices(b);
    /* ADR 0349: the cells consume the dirty range before the snapshot below clears it. */
    int use_cells = instance_frustum != NULL && mesh->bsphere_radius > 0.0f &&
                    instbatch_cells_refresh(b, mesh);
    {
        int64_t frame_serial = rt_canvas3d_get_frame_serial(canvas_obj);
        if (!b->motion_frame_initialized || b->last_motion_frame != frame_serial) {
            /* ADR 0349: after the swap the current buffer holds the snapshot from two frames
             * ago, so it differs from the live matrices only in the union of the last two
             * frames' dirty ranges. Anything structural or uninitialized copies everything. */
            int partial = b->motion_frame_initialized && b->has_prev_snapshot &&
                          b->motion_snapshot_count == b->instance_count &&
                          b->prev_count == b->instance_count && !b->dirty_all &&
                          !b->last_dirty_all;
            int32_t lo = 0;
            int32_t hi = -1;
            if (b->motion_snapshot_count > 0) {
                double *double_swap = b->owned_prev_transforms64;
                b->owned_prev_transforms64 = b->owned_current_snapshot64;
                b->owned_current_snapshot64 = double_swap;
                b->prev_transforms64 = b->owned_prev_transforms64;
                b->current_snapshot64 = b->owned_current_snapshot64;
                b->prev_count = b->motion_snapshot_count;
                b->has_prev_snapshot = 1;
            }
            if (partial) {
                int32_t a_lo;
                int32_t a_hi;
                int32_t b_lo;
                int32_t b_hi;
                int have_a = instbatch_dirty_span(
                    b->dirty_lo, b->dirty_hi, b->instance_count, &a_lo, &a_hi);
                int have_b = instbatch_dirty_span(
                    b->last_dirty_lo, b->last_dirty_hi, b->instance_count, &b_lo, &b_hi);
                if (have_a && have_b) {
                    lo = a_lo < b_lo ? a_lo : b_lo;
                    hi = a_hi > b_hi ? a_hi : b_hi;
                } else if (have_a) {
                    lo = a_lo;
                    hi = a_hi;
                } else if (have_b) {
                    lo = b_lo;
                    hi = b_hi;
                }
            } else {
                lo = 0;
                hi = b->instance_count - 1;
            }
            if (lo <= hi) {
                memcpy(&b->current_snapshot64[(size_t)lo * 16u],
                       &b->transforms64[(size_t)lo * 16u],
                       (size_t)(hi - lo + 1) * 16u * sizeof(double));
            }
            /* The previous float mirror (non-relative path) lags the previous snapshot by
             * exactly the range the previous frame applied. */
            if (b->prev_submit_valid && b->prev_submit_transforms && b->has_prev_snapshot &&
                b->prev_submit_capacity >= b->prev_count && !b->last_dirty_all) {
                int32_t m_lo;
                int32_t m_hi;
                if (instbatch_dirty_span(
                        b->last_dirty_lo, b->last_dirty_hi, b->prev_count, &m_lo, &m_hi)) {
                    for (int32_t i = m_lo; i <= m_hi; i++)
                        instbatch_copy_matrix64_to_float(
                            &b->prev_submit_transforms[(size_t)i * 16u],
                            &b->prev_transforms64[(size_t)i * 16u]);
                }
            } else {
                b->prev_submit_valid = 0;
            }
            b->last_dirty_lo = b->dirty_lo;
            b->last_dirty_hi = b->dirty_hi;
            b->last_dirty_all = b->dirty_all;
            b->dirty_lo = 0;
            b->dirty_hi = -1;
            b->dirty_all = 0;
            b->motion_snapshot_count = b->instance_count;
            b->last_motion_frame = frame_serial;
            b->motion_frame_initialized = 1;
        }
    }

    /* ADR 0349: retained cells classify the frustum per cell so outside cells (a parked
     * layer, the far end of a bowl) cost nothing, and every non-outside cell submits its own
     * compact segment, which also retires the canvas' per-frame counting sort. */
    {
        int camera_relative = canvas3d_uses_camera_relative_upload(c);
        int8_t has_prev = b->has_prev_snapshot && b->prev_count > 0 ? 1 : 0;
        const float *prev_floats = NULL;
        if (camera_relative) {
            if (!instbatch_ensure_matrix_scratch(
                    &b->visible_transforms, &b->visible_capacity, b->instance_count) ||
                (has_prev && !instbatch_ensure_matrix_scratch(&b->prev_submit_transforms,
                                                              &b->prev_submit_capacity,
                                                              b->instance_count))) {
                rt_trap("InstanceBatch3D.Draw: camera-relative matrix scratch allocation failed");
                return;
            }
            b->prev_submit_valid = 0; /* the scratch now holds frame-space matrices */
        } else {
            if (has_prev) {
                int32_t prev_capacity_before = b->prev_submit_capacity;
                if (instbatch_ensure_matrix_scratch(&b->prev_submit_transforms,
                                                    &b->prev_submit_capacity,
                                                    b->instance_count)) {
                    int32_t preserved =
                        b->prev_count < b->instance_count ? b->prev_count : b->instance_count;
                    /* The mirror persists across frames and is patched over the previous
                     * frame's dirty range at the snapshot; rebuild it only when it was
                     * invalidated, reallocated or the camera-relative path last owned it. */
                    if (!b->prev_submit_valid || b->prev_submit_capacity != prev_capacity_before) {
                        for (int32_t i = 0; i < preserved; i++) {
                            instbatch_copy_matrix64_to_float(
                                &b->prev_submit_transforms[(size_t)i * 16u],
                                &b->prev_transforms64[(size_t)i * 16u]);
                        }
                        b->prev_submit_valid = 1;
                    }
                    if (preserved < b->instance_count) {
                        memcpy(&b->prev_submit_transforms[(size_t)preserved * 16u],
                               &b->transforms[(size_t)preserved * 16u],
                               (size_t)(b->instance_count - preserved) * 16u * sizeof(float));
                    }
                    prev_floats = b->prev_submit_transforms;
                } else {
                    has_prev = 0;
                }
            }
            if (!instbatch_ensure_visibility_mask(b, b->instance_count) ||
                !instbatch_ensure_matrix_scratch(
                    &b->visible_transforms, &b->visible_capacity, b->instance_count) ||
                (has_prev && !instbatch_ensure_matrix_scratch(&b->visible_prev_transforms,
                                                              &b->visible_prev_capacity,
                                                              b->instance_count))) {
                /* No scratch: submit everything uncompacted, exactly as before. */
                rt_canvas3d_queue_instanced_batch_prepared(canvas_obj,
                                                           mesh,
                                                           mat,
                                                           b->transforms,
                                                           b->instance_count,
                                                           prev_floats,
                                                           has_prev,
                                                           NULL,
                                                           0);
                return;
            }
        }
        if (!use_cells) {
            instbatch_queue_segment(canvas_obj, c, b, mesh, mat, NULL, b->instance_count,
                                    instance_frustum, mesh_min, mesh_max, has_prev, prev_floats,
                                    camera_relative, 1);
            return;
        }
        for (int32_t cell = 0; cell < b->cell_count; cell++) {
            int32_t begin = b->cell_start[cell];
            int32_t count = b->cell_start[cell + 1] - begin;
            int cls;
            if (count <= 0)
                continue;
            cls = b->cell_bounded[cell]
                      ? instbatch_cell_classify(instance_frustum, b->cell_min[cell], b->cell_max[cell])
                      : 1;
            if (cls == 0) {
                if (!camera_relative && b->visibility_mask) {
                    for (int32_t k = begin; k < begin + count; k++)
                        b->visibility_mask[b->cell_members[k]] = 0u;
                }
                continue;
            }
            instbatch_queue_segment(canvas_obj, c, b, mesh, mat, &b->cell_members[begin], count,
                                    instance_frustum, mesh_min, mesh_max, has_prev, prev_floats,
                                    camera_relative, cls == 1);
        }
    }
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
