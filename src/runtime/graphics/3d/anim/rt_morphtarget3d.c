//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_morphtarget3d.c
// Purpose: MorphTarget3D — named blend shapes with per-vertex position, normal,
//   and tangent deltas. Applied on GPU when the active backend can carry the
//   payload, otherwise on CPU before draw submission.
//
// Key invariants:
//   - Deltas are stored as 3 floats per vertex per shape (sparse via zero default).
//   - Morph application: dst = base + sum(weight[i] * delta[i]) per vertex.
//   - Normals/tangents re-normalized after morph if any shape has corresponding deltas.
//   - Temporary morphed vertex buffer registered with canvas temp_buffers
//     to avoid use-after-free with deferred draw queue.
//   - GPU path is gated per-backend by `vgfx3d_backend_morph_shape_limit`.
//   - `payload_generation` skips zero on wrap so caches can use 0 as a sentinel.
//
// Ownership/Lifetime:
//   - MorphTarget3D is GC-managed; private allocation identities own every
//     top-level and per-shape buffer while legacy pointers remain repairable
//     mirrors. Finalization follows only those identities.
//
// Links: rt_morphtarget3d.h, plans/3d/16-morph-targets.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements morph-shape storage, persistence views, and draw-time deformation.
/// @details MorphTarget3D owns shape-major position deltas plus optional normal
///          and tangent channels, current and temporal weights, and lazily
///          packed GPU payloads. Drawing selects a supported GPU path when all
///          active data fit backend limits and otherwise builds a tracked
///          frame-lifetime CPU vertex buffer.

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_morphtarget3d.h"
#include "rt_alloc_size.h"
#include "rt_canvas3d.h"
#include "rt_canvas3d_internal.h"
#include "rt_g3d_ref_slots.h"
#include "rt_heap.h"
#include "rt_mat4.h"
#include "rt_morphtarget3d_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "vgfx3d_backend.h"
#include "vgfx3d_backend_d3d11_shared.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Largest finite-magnitude delta accepted when narrowing to float.
#define MORPHTARGET3D_FLOAT_ABS_MAX 3.40282346638528859812e38

/// @brief One named shape and its owned per-vertex delta channels.
typedef struct {
    /// Canonical UTF-8 byte name truncated to 63 bytes plus NUL.
    char name[64];
    /// Required `vertex_count * 3` position lanes.
    float *pos_deltas; /* 3 * vertex_count floats (dx, dy, dz per vertex) */
    /// Optional `vertex_count * 3` normal lanes.
    float *nrm_deltas; /* 3 * vertex_count floats (or NULL) */
    /// Optional `vertex_count * 3` tangent XYZ lanes; tangent W is unchanged.
    float *tan_deltas; /* 3 * vertex_count floats (or NULL); tangent.w is preserved */
    /// Immutable identity of the required position-delta allocation.
    float *owned_pos_deltas;
    /// Immutable identity of the optional normal-delta allocation.
    float *owned_nrm_deltas;
    /// Immutable identity of the optional tangent-delta allocation.
    float *owned_tan_deltas;
} vgfx3d_morph_shape_t;

/// @brief Private GC-managed MorphTarget3D representation.
typedef struct {
    /// Runtime object header / virtual table slot.
    void *vptr;
    /// Geometrically grown array of initialized shapes.
    vgfx3d_morph_shape_t *shapes;
    /// Current sanitized weights, index-aligned with @ref shapes.
    float *weights;
    /// Previous-frame weights exposed to motion-vector backends.
    float *prev_weights;
    /// Current-frame snapshot promoted on the next frame serial.
    float *motion_weight_snapshot;
    /// Lazy shape-major packed position payload.
    float *packed_pos_deltas;
    /// Lazy shape-major packed normal payload, or `NULL`.
    float *packed_nrm_deltas;
    /// Nonzero cache generation bumped by delta-channel mutation.
    uint64_t payload_generation;
    /// Payload generation represented by the maximum-delta cache.
    uint64_t max_delta_generation;
    /// Cached largest authored position-delta vector length.
    double max_position_delta_cache;
    /// Logical initialized shape count.
    int32_t shape_count;
    /// Allocated slots in each index-aligned array.
    int32_t shape_capacity;
    /// Fixed vertex count for every shape channel.
    int32_t vertex_count;
    /// Canvas frame serial at which motion history last advanced.
    int64_t last_motion_frame;
    /// Cached candidate index for name lookup, always verified before use.
    int32_t name_lookup_memo; /* index of the last name-lookup hit (verified before use) */
    /// Nonzero once @ref prev_weights contains a prior-frame snapshot.
    int8_t has_prev_weights;
    /// Nonzero when packed position/normal arrays must be rebuilt.
    int8_t packed_dirty;
    /// Immutable identity of the shape-record allocation.
    vgfx3d_morph_shape_t *owned_shapes;
    /// Immutable identity of the current-weight allocation.
    float *owned_weights;
    /// Immutable identity of the previous-weight allocation.
    float *owned_prev_weights;
    /// Immutable identity of the motion-snapshot allocation.
    float *owned_motion_weight_snapshot;
    /// Immutable identity of the packed position allocation.
    float *owned_packed_pos_deltas;
    /// Immutable identity of the packed normal allocation.
    float *owned_packed_nrm_deltas;
    /// Actual slot count allocated in all four aligned top-level arrays.
    int32_t allocation_shape_capacity;
    /// Actual vertex count represented by every per-shape channel allocation.
    int32_t allocation_vertex_count;
    /// Number of shape records successfully initialized and published.
    int32_t initialized_shape_count;
    /// Distinguishes a real frame-serial zero from uninitialized history.
    int8_t motion_frame_initialized;
} rt_morphtarget3d;

/// @brief Validate @p obj is a heap-allocated Mat4 and return its typed pointer (NULL on mismatch).
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed Mat4 payload pointer, or `NULL` when the pointer, heap
///         payload, or class identifier is invalid.
static mat4_impl *morphtarget_mat4_checked(void *obj) {
    if (!obj || !rt_heap_is_payload(obj) || rt_obj_class_id(obj) != RT_MAT4_CLASS_ID)
        return NULL;
    return (mat4_impl *)obj;
}

/// @brief Flag the packed GPU payload dirty and bump the generation counter.
/// @details Called after any per-shape delta array mutation so downstream
///   consumers (GPU backends that cache the flat payload as a vertex/constant
///   buffer) know to re-upload on the next draw. The generation counter
///   wraps from UINT64_MAX back to 1 rather than 0 so "0" remains a reliable
///   "never seen" sentinel in caches that compare against the last-known value.
/// @param[in,out] mt Morph target whose packed payload and generation to
///                   invalidate.
static void morphtarget_touch_payload(rt_morphtarget3d *mt) {
    if (!mt)
        return;
    mt->packed_dirty = 1;
    if (mt->payload_generation == UINT64_MAX)
        mt->payload_generation = 1;
    else
        mt->payload_generation++;
    mt->max_delta_generation = 0;
}

/// @brief Validate @p obj as a MorphTarget3D handle and return its typed pointer (NULL on
/// mismatch).
/// @param[in] obj Opaque borrowed runtime object handle.
/// @return Borrowed typed morph-target pointer, or `NULL` on class/liveness
///         mismatch.
static rt_morphtarget3d *morphtarget_checked(void *obj) {
    return (rt_morphtarget3d *)rt_g3d_checked_or_null(obj, RT_G3D_MORPHTARGET3D_CLASS_ID);
}

/// @brief Restore top-level storage mirrors and canonicalize container metadata.
/// @param[in,out] mt Morph target whose private allocation state is authoritative.
/// @return One when fixed vertex storage and all allocated aligned arrays are usable.
static int morphtarget_repair_storage(rt_morphtarget3d *mt) {
    if (!mt || mt->allocation_vertex_count <= 0)
        return 0;
    mt->vertex_count = mt->allocation_vertex_count;
    if (mt->allocation_shape_capacity < 0)
        return 0;
    if (mt->allocation_shape_capacity == 0) {
        mt->shapes = NULL;
        mt->weights = NULL;
        mt->prev_weights = NULL;
        mt->motion_weight_snapshot = NULL;
        mt->shape_capacity = 0;
        mt->shape_count = 0;
        mt->initialized_shape_count = 0;
    } else {
        if (!mt->owned_shapes || !mt->owned_weights || !mt->owned_prev_weights ||
            !mt->owned_motion_weight_snapshot)
            return 0;
        mt->shapes = mt->owned_shapes;
        mt->weights = mt->owned_weights;
        mt->prev_weights = mt->owned_prev_weights;
        mt->motion_weight_snapshot = mt->owned_motion_weight_snapshot;
        mt->shape_capacity = mt->allocation_shape_capacity;
        if (mt->initialized_shape_count < 0)
            mt->initialized_shape_count = 0;
        if (mt->initialized_shape_count > mt->allocation_shape_capacity)
            mt->initialized_shape_count = mt->allocation_shape_capacity;
        mt->shape_count = mt->initialized_shape_count;
    }
    mt->packed_pos_deltas = mt->owned_packed_pos_deltas;
    mt->packed_nrm_deltas = mt->owned_packed_nrm_deltas;
    mt->packed_dirty = mt->packed_dirty ? 1 : 0;
    mt->motion_frame_initialized = mt->motion_frame_initialized ? 1 : 0;
    mt->has_prev_weights = mt->has_prev_weights ? 1 : 0;
    if (!mt->motion_frame_initialized)
        mt->has_prev_weights = 0;
    if (mt->payload_generation == 0) {
        mt->payload_generation = 1;
        mt->packed_dirty = 1;
        mt->max_delta_generation = 0;
        mt->max_position_delta_cache = 0.0;
    }
    return 1;
}

/// @brief Restore one shape's public channel mirrors from its owned identities.
/// @param[in,out] shape Shape record stored inside the owned table.
/// @return One when required position storage exists; otherwise zero.
static int morphtarget_repair_shape(vgfx3d_morph_shape_t *shape) {
    if (!shape)
        return 0;
    shape->name[sizeof(shape->name) - 1u] = '\0';
    shape->pos_deltas = shape->owned_pos_deltas;
    shape->nrm_deltas = shape->owned_nrm_deltas;
    shape->tan_deltas = shape->owned_tan_deltas;
    return shape->owned_pos_deltas ? 1 : 0;
}

/// @brief Return the initialized readable shape prefix after repairing every record mirror.
/// @param[in,out] mt Morph target and aligned arrays to inspect.
/// @return Safe readable shape-prefix length, or zero.
static int32_t morphtarget_safe_shape_count(rt_morphtarget3d *mt) {
    int32_t limit;
    int32_t count = 0;
    if (!morphtarget_repair_storage(mt) || mt->shape_count <= 0 || mt->shape_capacity <= 0)
        return 0;
    limit = mt->shape_count < mt->shape_capacity ? mt->shape_count : mt->shape_capacity;
    while (count < limit && morphtarget_repair_shape(&mt->shapes[count]))
        count++;
    if (mt->shape_count != count)
        mt->shape_count = count;
    return count;
}

/// @brief Clamp the morph-target's shape_count to its safe value (defensive).
/// @param[in,out] mt Morph target whose parallel-array metadata to repair.
static void morphtarget_repair_shape_table(rt_morphtarget3d *mt) {
    if (!mt)
        return;
    mt->shape_count = morphtarget_safe_shape_count(mt);
}

/// @brief Compute the total float count for `shape_count × vertex_count × 3` delta storage.
/// @details Returns 0 on any overflow in the multiply chain, 1 on success with @p out_count
///          populated. Used to size the packed GPU delta buffer safely.
/// @param[in] shape_count Nonnegative number of shapes.
/// @param[in] vertex_count Positive vertices per shape.
/// @param[out] out_count Destination for the total number of float lanes.
/// @return `1` when multiplication is valid; `0` for invalid input or
///         `size_t` overflow.
static int morphtarget_delta_float_count(int32_t shape_count,
                                         int32_t vertex_count,
                                         size_t *out_count) {
    if (!out_count || shape_count < 0 || vertex_count <= 0)
        return 0;
    size_t count = (size_t)shape_count;
    if (count > SIZE_MAX / (size_t)vertex_count)
        return 0;
    count *= (size_t)vertex_count;
    if (count > SIZE_MAX / 3u)
        return 0;
    count *= 3u;
    *out_count = count;
    return 1;
}

/// @brief Byte size of a single shape's vertex-delta array (`vertex_count × 3 × sizeof(float)`),
///        with overflow check.
/// @param[in] vertex_count Positive vertices in the shape.
/// @param[out] out_bytes Destination for the byte size.
/// @return `1` on success; `0` for invalid input or size overflow.
static int morphtarget_vertex_delta_bytes(int32_t vertex_count, size_t *out_bytes) {
    size_t float_count;
    if (!morphtarget_delta_float_count(1, vertex_count, &float_count))
        return 0;
    if (float_count > SIZE_MAX / sizeof(float))
        return 0;
    *out_bytes = float_count * sizeof(float);
    return 1;
}

/// @brief Coerce a delta sample to float; non-finite inputs collapse to 0.
/// @param[in] value Double-precision channel lane to sanitize.
/// @return Finite float value, or zero when non-finite/outside float range.
static float morphtarget_sanitize_delta(double value) {
    return (isfinite(value) && value >= -MORPHTARGET3D_FLOAT_ABS_MAX &&
            value <= MORPHTARGET3D_FLOAT_ABS_MAX)
               ? (float)value
               : 0.0f;
}

/// @brief Clamp a morph weight to the runtime-supported [-1, 1] range.
/// @param[in] weight Requested signed blend contribution.
/// @return Finite float in `[-1,1]`; non-finite input becomes zero.
static float morphtarget_sanitize_weight(double weight) {
    if (!isfinite(weight))
        return 0.0f;
    if (weight < -1.0)
        return -1.0f;
    if (weight > 1.0)
        return 1.0f;
    return (float)weight;
}

/// @brief Copy a float channel while canonicalizing every lane.
/// @param[out] dst Writable destination with @p value_count lanes.
/// @param[in] src Borrowed source with @p value_count lanes.
/// @param[in] value_count Number of float lanes to copy.
static void morphtarget_copy_sanitized_channel(float *dst, const float *src, size_t value_count) {
    if (!dst || !src)
        return;
    for (size_t i = 0; i < value_count; ++i)
        dst[i] = morphtarget_sanitize_delta(src[i]);
}

/// @brief Bounded byte length for an externally supplied C string.
/// @param[in] text Borrowed C string.
/// @param[in] limit Maximum bytes examined before the terminator.
/// @return Terminator offset, or @p limit when no terminator occurs in range.
static size_t morphtarget_bounded_cstr_length(const char *text, size_t limit) {
    size_t length = 0;
    if (!text)
        return limit;
    while (length < limit && text[length] != '\0')
        length++;
    return length;
}

/// @brief Euclidean length of a 3-float morph delta (as a double); 0 when any lane is non-finite.
/// @param[in] delta Borrowed three-float delta vector.
/// @return Finite Euclidean length, or zero.
static double morphtarget_delta_length_or_zero(const float *delta) {
    double x;
    double y;
    double z;
    double len;
    if (!delta || !isfinite(delta[0]) || !isfinite(delta[1]) || !isfinite(delta[2]))
        return 0.0;
    x = fabs((double)delta[0]);
    y = fabs((double)delta[1]);
    z = fabs((double)delta[2]);
    len = hypot(hypot(x, y), z);
    return isfinite(len) ? len : 0.0;
}

/// @brief Normalize a 3-float direction, or restore the base direction when it degenerates.
/// @param[in,out] direction Writable three-float vector; `NULL` is ignored.
/// @param[in] fallback Optional borrowed three-float replacement sanitized and
///                     normalized when @p direction is degenerate.
static void morphtarget_normalize3_or_copy(float *direction, const float *fallback) {
    double x;
    double y;
    double z;
    double len;
    if (!direction)
        return;
    x = isfinite(direction[0]) ? (double)direction[0] : 0.0;
    y = isfinite(direction[1]) ? (double)direction[1] : 0.0;
    z = isfinite(direction[2]) ? (double)direction[2] : 0.0;
    len = hypot(hypot(x, y), z);
    if (isfinite(len) && len > 1e-8) {
        direction[0] = (float)(x / len);
        direction[1] = (float)(y / len);
        direction[2] = (float)(z / len);
        return;
    }
    if (fallback) {
        x = isfinite(fallback[0]) ? (double)fallback[0] : 0.0;
        y = isfinite(fallback[1]) ? (double)fallback[1] : 0.0;
        z = isfinite(fallback[2]) ? (double)fallback[2] : 0.0;
        len = hypot(hypot(x, y), z);
        if (isfinite(len) && len > 1e-8) {
            direction[0] = (float)(x / len);
            direction[1] = (float)(y / len);
            direction[2] = (float)(z / len);
            return;
        }
    }
    direction[0] = 0.0f;
    direction[1] = 0.0f;
    direction[2] = 0.0f;
}

/// @brief Sanitize all weight history arrays before they are exposed to draw backends.
/// @param[in,out] mt Morph target whose valid current, previous, and snapshot
///                   weights to clamp in place.
static void morphtarget_sanitize_weights(rt_morphtarget3d *mt) {
    if (!mt)
        return;
    for (int32_t i = 0, count = morphtarget_safe_shape_count(mt); i < count; ++i) {
        if (mt->weights)
            mt->weights[i] = morphtarget_sanitize_weight(mt->weights[i]);
        if (mt->prev_weights)
            mt->prev_weights[i] = morphtarget_sanitize_weight(mt->prev_weights[i]);
        if (mt->motion_weight_snapshot)
            mt->motion_weight_snapshot[i] =
                morphtarget_sanitize_weight(mt->motion_weight_snapshot[i]);
    }
}

/// @brief Re-linearize the per-shape delta arrays into a single GPU-friendly buffer.
/// @details Blend shapes arrive from authoring tools as independent arrays
///   (one `pos_deltas` / `nrm_deltas` allocation per shape). GPU backends,
///   however, want a single flat `shape_count * vertex_count * 3` buffer
///   they can bind once. This routine concatenates them and replaces any
///   previously-built packed arrays. Normal deltas are optional and only
///   allocated when at least one shape supplies them, saving memory for
///   position-only morphs (blink, phoneme shapes, etc.).
/// @param[in,out] mt Morph target whose packed arrays to rebuild transactionally.
/// @return 1 on success (or no-op when `packed_dirty == 0`), 0 on OOM — the
///   caller should skip the draw rather than render with stale deltas.
static int morphtarget_rebuild_packed_payload(rt_morphtarget3d *mt) {
    size_t delta_count;
    float *packed_pos = NULL;
    float *packed_nrm = NULL;
    int has_normal_deltas = 0;

    if (!mt)
        return 0;
    morphtarget_repair_shape_table(mt);
    if (!mt->packed_dirty)
        return 1;

    if (!morphtarget_delta_float_count(mt->shape_count, mt->vertex_count, &delta_count))
        return 0;
    if (delta_count > 0)
        packed_pos = (float *)calloc(delta_count, sizeof(float));
    for (int32_t s = 0; s < mt->shape_count; s++) {
        if (mt->shapes[s].nrm_deltas) {
            has_normal_deltas = 1;
            break;
        }
    }
    if (delta_count > 0 && has_normal_deltas)
        packed_nrm = (float *)calloc(delta_count, sizeof(float));
    if (delta_count > 0 && (!packed_pos || (has_normal_deltas && !packed_nrm))) {
        free(packed_pos);
        free(packed_nrm);
        return 0;
    }

    for (int32_t s = 0; s < mt->shape_count; s++) {
        size_t offset = (size_t)s * (size_t)mt->vertex_count * 3u;
        if (packed_pos && mt->shapes[s].pos_deltas) {
            for (size_t i = 0; i < (size_t)mt->vertex_count * 3u; ++i)
                packed_pos[offset + i] = morphtarget_sanitize_delta(mt->shapes[s].pos_deltas[i]);
        }
        if (packed_nrm && mt->shapes[s].nrm_deltas) {
            for (size_t i = 0; i < (size_t)mt->vertex_count * 3u; ++i)
                packed_nrm[offset + i] = morphtarget_sanitize_delta(mt->shapes[s].nrm_deltas[i]);
        }
    }

    free(mt->owned_packed_pos_deltas);
    free(mt->owned_packed_nrm_deltas);
    mt->owned_packed_pos_deltas = packed_pos;
    mt->owned_packed_nrm_deltas = packed_nrm;
    mt->packed_pos_deltas = packed_pos;
    mt->packed_nrm_deltas = packed_nrm;
    mt->packed_dirty = 0;
    return 1;
}

/// @brief Number of shape slots the finalizer can walk even if parallel arrays are corrupt.
/// @param[in] mt Morph target whose primary shape allocation to inspect.
/// @return Safe number of shape records whose owned channels may be freed.
static int32_t morphtarget_finalize_shape_count(const rt_morphtarget3d *mt) {
    return mt && mt->owned_shapes && mt->allocation_shape_capacity > 0
               ? mt->allocation_shape_capacity
               : 0;
}

/// @brief Grow the parallel shape / weight / prev-weight / motion arrays.
/// @details All four arrays must remain index-aligned, so they're reallocated
///   together under a single transaction — failure to allocate any one of
///   them frees the rest before returning, preventing a torn state where
///   `shapes[]` has more entries than `weights[]`. Capacity doubles from an
///   initial 4 slots until `min_capacity` fits, with an INT32_MAX/2 guard
///   against integer overflow.
/// @param[in,out] mt Morph target whose aligned arrays to replace.
/// @param[in] min_capacity Minimum required number of shape slots.
/// @return 1 if capacity >= min_capacity (possibly no-op), 0 on OOM.
static int morphtarget_reserve_shapes(rt_morphtarget3d *mt, int32_t min_capacity) {
    int32_t new_capacity;
    vgfx3d_morph_shape_t *new_shapes = NULL;
    float *new_weights = NULL;
    float *new_prev_weights = NULL;
    float *new_motion_snapshot = NULL;

    if (!mt || !morphtarget_repair_storage(mt))
        return 0;
    morphtarget_repair_shape_table(mt);
    if (min_capacity < 0 || mt->shape_count != mt->initialized_shape_count)
        return 0;
    if (min_capacity <= mt->allocation_shape_capacity)
        return 1;

    new_capacity = mt->allocation_shape_capacity > 0 ? mt->allocation_shape_capacity : 4;
    while (new_capacity < min_capacity) {
        if (new_capacity > INT32_MAX / 2) {
            new_capacity = min_capacity;
            break;
        }
        new_capacity *= 2;
    }
    if ((size_t)new_capacity > SIZE_MAX / sizeof(*new_shapes) ||
        (size_t)new_capacity > SIZE_MAX / sizeof(*new_weights))
        return 0;

    new_shapes = (vgfx3d_morph_shape_t *)calloc((size_t)new_capacity, sizeof(*new_shapes));
    new_weights = (float *)calloc((size_t)new_capacity, sizeof(*new_weights));
    new_prev_weights = (float *)calloc((size_t)new_capacity, sizeof(*new_prev_weights));
    new_motion_snapshot = (float *)calloc((size_t)new_capacity, sizeof(*new_motion_snapshot));
    if (!new_shapes || !new_weights || !new_prev_weights || !new_motion_snapshot) {
        free(new_shapes);
        free(new_weights);
        free(new_prev_weights);
        free(new_motion_snapshot);
        return 0;
    }

    if (mt->shape_count > 0) {
        if (mt->owned_shapes)
            memcpy(new_shapes, mt->owned_shapes, (size_t)mt->shape_count * sizeof(*new_shapes));
        if (mt->owned_weights)
            memcpy(new_weights, mt->owned_weights, (size_t)mt->shape_count * sizeof(*new_weights));
        if (mt->owned_prev_weights)
            memcpy(new_prev_weights,
                   mt->owned_prev_weights,
                   (size_t)mt->shape_count * sizeof(*new_prev_weights));
        if (mt->owned_motion_weight_snapshot)
            memcpy(new_motion_snapshot,
                   mt->owned_motion_weight_snapshot,
                   (size_t)mt->shape_count * sizeof(*new_motion_snapshot));
    }

    free(mt->owned_shapes);
    free(mt->owned_weights);
    free(mt->owned_prev_weights);
    free(mt->owned_motion_weight_snapshot);
    mt->owned_shapes = new_shapes;
    mt->owned_weights = new_weights;
    mt->owned_prev_weights = new_prev_weights;
    mt->owned_motion_weight_snapshot = new_motion_snapshot;
    mt->shapes = new_shapes;
    mt->weights = new_weights;
    mt->prev_weights = new_prev_weights;
    mt->motion_weight_snapshot = new_motion_snapshot;
    mt->shape_capacity = new_capacity;
    mt->allocation_shape_capacity = new_capacity;
    return 1;
}

/// @brief Report the maximum morph shape count the named backend can handle on-GPU.
/// @details The limits mirror each backend's shader-side weight-array budget
///   (`VGFX3D_*_MAX_MORPH_SHAPES`, 64 shapes) so any mesh beyond the budget takes
///   the CPU blending fallback instead of silently dropping overflow shapes —
///   the Metal draw path clamps to its constant, so reporting a larger limit
///   here would lose shapes past the clamp. Unknown backends (software renderer,
///   disabled builds) return 0 so the caller falls back to CPU deformation.
/// @param[in] backend_name Borrowed canonical backend name.
/// @return Supported GPU shape count, or zero for unknown/CPU-only backends.
static int32_t vgfx3d_backend_morph_shape_limit(const char *backend_name) {
    if (!backend_name)
        return 0;
    if (strcmp(backend_name, "metal") == 0)
        return 64; /* VGFX3D_METAL_MAX_MORPH_SHAPES */
    if (strcmp(backend_name, "opengl") == 0)
        return 64; /* VGFX3D_OPENGL_MAX_MORPH_SHAPES */
    if (strcmp(backend_name, "d3d11") == 0)
        return VGFX3D_D3D11_MAX_MORPH_SHAPES;
    return 0;
}

/// @brief Decide whether to dispatch GPU-side morph blending for this draw.
/// @details Any mesh whose `shape_count` exceeds the backend's shader payload
///   capacity must fall back to CPU blending instead, because partial GPU
///   evaluation would silently drop the overflow shapes. Checked per draw,
///   not once at mesh load, so a morph that starts small can stay on the
///   GPU path even if other morph assets on the same canvas need CPU blending.
/// @param[in] backend_name Borrowed backend name.
/// @param[in] shape_count Number of morph shapes required by this draw.
/// @return Non-zero when GPU morph is viable.
static int vgfx3d_backend_prefers_gpu_morph(const char *backend_name, int32_t shape_count) {
    int32_t limit = vgfx3d_backend_morph_shape_limit(backend_name);
    return limit > 0 && shape_count <= limit;
}

/*==========================================================================
 * Lifecycle
 *=========================================================================*/

/// @brief GC finalizer — release every per-shape delta array and packed buffer.
/// @details Walks `shapes[0..shape_count]` first to drop their owned
///   `pos_deltas` / `nrm_deltas`, then releases the four index-aligned
///   top-level arrays plus the packed GPU payload allocations. Order matters
///   only for debuggability — nothing here has cross-allocation dependencies,
///   so OOM during any intermediate alloc elsewhere in the file is safe
///   because finalize is idempotent against null slots.
/// @param[in,out] obj MorphTarget3D storage being finalized; `NULL` is ignored.
static void rt_morphtarget3d_finalize(void *obj) {
    rt_morphtarget3d *mt = (rt_morphtarget3d *)obj;
    if (!mt)
        return;
    for (int32_t i = 0, count = morphtarget_finalize_shape_count(mt); i < count; i++) {
        free(mt->owned_shapes[i].owned_pos_deltas);
        free(mt->owned_shapes[i].owned_nrm_deltas);
        free(mt->owned_shapes[i].owned_tan_deltas);
    }
    free(mt->owned_shapes);
    free(mt->owned_weights);
    free(mt->owned_prev_weights);
    free(mt->owned_motion_weight_snapshot);
    free(mt->owned_packed_pos_deltas);
    free(mt->owned_packed_nrm_deltas);
    mt->shapes = NULL;
    mt->weights = NULL;
    mt->prev_weights = NULL;
    mt->motion_weight_snapshot = NULL;
    mt->packed_pos_deltas = NULL;
    mt->packed_nrm_deltas = NULL;
    mt->owned_shapes = NULL;
    mt->owned_weights = NULL;
    mt->owned_prev_weights = NULL;
    mt->owned_motion_weight_snapshot = NULL;
    mt->owned_packed_pos_deltas = NULL;
    mt->owned_packed_nrm_deltas = NULL;
    mt->shape_count = 0;
    mt->shape_capacity = 0;
    mt->vertex_count = 0;
    mt->allocation_shape_capacity = 0;
    mt->allocation_vertex_count = 0;
    mt->initialized_shape_count = 0;
    mt->has_prev_weights = 0;
    mt->packed_dirty = 0;
    mt->motion_frame_initialized = 0;
}

/// @brief Create a morph target container for blendshape animation.
/// @details Morph targets (aka blend shapes) deform a mesh by adding weighted
///          per-vertex deltas to the base positions and normals. Shapes grow on
///          demand, while GPU backends may still fall back to CPU deformation if
///          the active shape count exceeds the backend's shader payload limits.
/// @param vertex_count Number of vertices in the base mesh (must match mesh vertex count).
/// @return Opaque morph target handle, or NULL on failure.
void *rt_morphtarget3d_new(int64_t vertex_count) {
    if (vertex_count <= 0 || vertex_count > INT32_MAX ||
        (uint64_t)vertex_count > (uint64_t)(SIZE_MAX / (3u * sizeof(float)))) {
        rt_trap("MorphTarget3D.New: vertex_count out of range");
        return NULL;
    }
    rt_morphtarget3d *mt = (rt_morphtarget3d *)rt_obj_new_i64(RT_G3D_MORPHTARGET3D_CLASS_ID,
                                                              (int64_t)sizeof(rt_morphtarget3d));
    if (!mt) {
        rt_trap("MorphTarget3D.New: memory allocation failed");
        return NULL;
    }
    mt->vptr = NULL;
    mt->shapes = NULL;
    mt->weights = NULL;
    mt->prev_weights = NULL;
    mt->motion_weight_snapshot = NULL;
    mt->shape_count = 0;
    mt->shape_capacity = 0;
    mt->vertex_count = (int32_t)vertex_count;
    mt->allocation_shape_capacity = 0;
    mt->allocation_vertex_count = (int32_t)vertex_count;
    mt->initialized_shape_count = 0;
    mt->last_motion_frame = 0;
    mt->has_prev_weights = 0;
    mt->packed_pos_deltas = NULL;
    mt->packed_nrm_deltas = NULL;
    mt->payload_generation = 1;
    mt->max_delta_generation = 0;
    mt->max_position_delta_cache = 0.0;
    mt->name_lookup_memo = -1;
    mt->packed_dirty = 1;
    rt_obj_set_finalizer(mt, rt_morphtarget3d_finalize);
    return mt;
}

/// @brief Create a deep copy of a morph target, duplicating all shape delta buffers.
/// @param obj Source morph target handle.
/// @return New morph target handle with identical shape data, or NULL on failure.
void *rt_morphtarget3d_clone(void *obj) {
    rt_morphtarget3d *src = morphtarget_checked(obj);
    rt_morphtarget3d *dst;
    if (!src || !morphtarget_repair_storage(src))
        return NULL;
    dst = (rt_morphtarget3d *)rt_morphtarget3d_new(src->vertex_count);
    if (!dst)
        return NULL;
    size_t delta_size;
    if (!morphtarget_vertex_delta_bytes(src->vertex_count, &delta_size)) {
        if (rt_obj_release_check0(dst))
            rt_obj_free(dst);
        return NULL;
    }
    for (int32_t i = 0, count = morphtarget_safe_shape_count(src); i < count; i++) {
        rt_string shape_name = rt_const_cstr(src->shapes[i].name);
        int64_t shape = rt_morphtarget3d_add_shape(dst, shape_name);
        rt_string_unref(shape_name);
        if (shape < 0) {
            if (rt_obj_release_check0(dst))
                rt_obj_free(dst);
            return NULL;
        }
        if (src->shapes[i].pos_deltas && dst->shapes[shape].pos_deltas) {
            morphtarget_copy_sanitized_channel(dst->shapes[shape].pos_deltas,
                                               src->shapes[i].pos_deltas,
                                               delta_size / sizeof(float));
        }
        if (src->shapes[i].nrm_deltas) {
            dst->shapes[shape].owned_nrm_deltas = (float *)calloc(1, delta_size);
            dst->shapes[shape].nrm_deltas = dst->shapes[shape].owned_nrm_deltas;
            if (!dst->shapes[shape].owned_nrm_deltas) {
                rt_trap("MorphTarget3D.Clone: normal-delta allocation failed");
                if (rt_obj_release_check0(dst))
                    rt_obj_free(dst);
                return NULL;
            }
            morphtarget_copy_sanitized_channel(dst->shapes[shape].nrm_deltas,
                                               src->shapes[i].nrm_deltas,
                                               delta_size / sizeof(float));
        }
        if (src->shapes[i].tan_deltas) {
            dst->shapes[shape].owned_tan_deltas = (float *)calloc(1, delta_size);
            dst->shapes[shape].tan_deltas = dst->shapes[shape].owned_tan_deltas;
            if (!dst->shapes[shape].owned_tan_deltas) {
                rt_trap("MorphTarget3D.Clone: tangent-delta allocation failed");
                if (rt_obj_release_check0(dst))
                    rt_obj_free(dst);
                return NULL;
            }
            morphtarget_copy_sanitized_channel(dst->shapes[shape].tan_deltas,
                                               src->shapes[i].tan_deltas,
                                               delta_size / sizeof(float));
        }
        dst->weights[shape] = src->weights ? morphtarget_sanitize_weight(src->weights[i]) : 0.0f;
        dst->prev_weights[shape] =
            src->prev_weights ? morphtarget_sanitize_weight(src->prev_weights[i]) : 0.0f;
        dst->motion_weight_snapshot[shape] =
            src->motion_weight_snapshot
                ? morphtarget_sanitize_weight(src->motion_weight_snapshot[i])
                : dst->weights[shape];
    }
    dst->has_prev_weights = src->has_prev_weights ? 1 : 0;
    dst->last_motion_frame = src->last_motion_frame;
    dst->motion_frame_initialized = src->motion_frame_initialized ? 1 : 0;
    if (!dst->motion_frame_initialized)
        dst->has_prev_weights = 0;
    morphtarget_touch_payload(dst);
    return dst;
}

/**
 * @brief Deep-copy all morph channels through an output-to-source vertex map.
 *
 * The complete map is validated before the destination is allocated. Every shape
 * then receives exact subset copies of its position channel and any optional normal
 * and tangent channels. Shape metadata and all three weight-history arrays remain
 * index-aligned, allowing a simplified Mesh3D to retain animation semantics without
 * referencing source-sized delta buffers.
 *
 * @param obj Source MorphTarget3D handle.
 * @param new_to_old Borrowed array containing one valid source vertex index per output vertex.
 * @param new_vertex_count Number of output vertices/map entries; must be positive and fit int32.
 * @return A new independently owned MorphTarget3D, or `NULL` on validation/allocation failure.
 */
void *rt_morphtarget3d_clone_remapped(void *obj,
                                      const uint32_t *new_to_old,
                                      uint32_t new_vertex_count) {
    rt_morphtarget3d *src = morphtarget_checked(obj);
    rt_morphtarget3d *dst = NULL;
    size_t delta_size;
    int32_t shape_count;

    if (!src || !morphtarget_repair_storage(src) || !new_to_old || new_vertex_count == 0 ||
        new_vertex_count > (uint32_t)INT32_MAX || src->vertex_count <= 0)
        return NULL;
    morphtarget_repair_shape_table(src);
    for (uint32_t vertex = 0; vertex < new_vertex_count; ++vertex) {
        if (new_to_old[vertex] >= (uint32_t)src->vertex_count)
            return NULL;
    }
    if (!morphtarget_vertex_delta_bytes((int32_t)new_vertex_count, &delta_size))
        return NULL;

    dst = (rt_morphtarget3d *)rt_morphtarget3d_new((int64_t)new_vertex_count);
    if (!dst)
        return NULL;
    shape_count = morphtarget_safe_shape_count(src);
    for (int32_t source_shape = 0; source_shape < shape_count; ++source_shape) {
        rt_string shape_name = rt_const_cstr(src->shapes[source_shape].name);
        int64_t output_shape = rt_morphtarget3d_add_shape(dst, shape_name);
        rt_string_unref(shape_name);
        if (output_shape < 0)
            goto fail;

        if (src->shapes[source_shape].nrm_deltas) {
            dst->shapes[output_shape].owned_nrm_deltas = (float *)calloc(1, delta_size);
            dst->shapes[output_shape].nrm_deltas = dst->shapes[output_shape].owned_nrm_deltas;
            if (!dst->shapes[output_shape].owned_nrm_deltas) {
                rt_trap("MorphTarget3D.CloneRemapped: normal-delta allocation failed");
                goto fail;
            }
        }
        if (src->shapes[source_shape].tan_deltas) {
            dst->shapes[output_shape].owned_tan_deltas = (float *)calloc(1, delta_size);
            dst->shapes[output_shape].tan_deltas = dst->shapes[output_shape].owned_tan_deltas;
            if (!dst->shapes[output_shape].owned_tan_deltas) {
                rt_trap("MorphTarget3D.CloneRemapped: tangent-delta allocation failed");
                goto fail;
            }
        }
        for (uint32_t output_vertex = 0; output_vertex < new_vertex_count; ++output_vertex) {
            size_t source_offset = (size_t)new_to_old[output_vertex] * 3u;
            size_t output_offset = (size_t)output_vertex * 3u;
            morphtarget_copy_sanitized_channel(&dst->shapes[output_shape].pos_deltas[output_offset],
                                               &src->shapes[source_shape].pos_deltas[source_offset],
                                               3u);
            if (dst->shapes[output_shape].nrm_deltas) {
                morphtarget_copy_sanitized_channel(
                    &dst->shapes[output_shape].nrm_deltas[output_offset],
                    &src->shapes[source_shape].nrm_deltas[source_offset],
                    3u);
            }
            if (dst->shapes[output_shape].tan_deltas) {
                morphtarget_copy_sanitized_channel(
                    &dst->shapes[output_shape].tan_deltas[output_offset],
                    &src->shapes[source_shape].tan_deltas[source_offset],
                    3u);
            }
        }
        dst->weights[output_shape] =
            src->weights ? morphtarget_sanitize_weight(src->weights[source_shape]) : 0.0f;
        dst->prev_weights[output_shape] =
            src->prev_weights ? morphtarget_sanitize_weight(src->prev_weights[source_shape]) : 0.0f;
        dst->motion_weight_snapshot[output_shape] =
            src->motion_weight_snapshot
                ? morphtarget_sanitize_weight(src->motion_weight_snapshot[source_shape])
                : dst->weights[output_shape];
    }
    dst->has_prev_weights = src->has_prev_weights ? 1 : 0;
    dst->last_motion_frame = src->last_motion_frame;
    dst->motion_frame_initialized = src->motion_frame_initialized ? 1 : 0;
    if (!dst->motion_frame_initialized)
        dst->has_prev_weights = 0;
    morphtarget_touch_payload(dst);
    return dst;

fail:
    if (rt_obj_release_check0(dst))
        rt_obj_free(dst);
    return NULL;
}

/*==========================================================================
 * Shape management
 *=========================================================================*/

/// @brief Copy a runtime string into the fixed blend-shape name storage.
/// @details A null string becomes an empty canonical name; valid strings are
///          truncated to 63 bytes and NUL-terminated.
/// @param[in] name Borrowed runtime string handle, or `NULL`.
/// @param[out] out Writable 64-byte destination.
/// @return `1` when a canonical name was produced; `0` for an invalid string
///         handle or output.
static int morphtarget_copy_canonical_name(rt_string name, char out[64]) {
    const char *cstr;
    size_t len;
    if (!out)
        return 0;
    out[0] = '\0';
    if (!name)
        return 1;
    if (!rt_string_is_handle(name))
        return 0;
    cstr = rt_string_cstr(name);
    if (!cstr)
        return 0;
    len = strlen(cstr);
    if (len > 63)
        len = 63;
    memcpy(out, cstr, len);
    out[len] = '\0';
    return 1;
}

/// @brief Register a named blend shape and allocate its per-vertex delta arrays.
/// @details Names are copied into 64-byte storage and need not be unique.
///          Position deltas are zero-initialized immediately; optional normal
///          and tangent arrays remain lazy. Allocation failure traps where
///          applicable and leaves the logical shape count unchanged.
/// @param[in,out] obj MorphTarget3D to extend.
/// @param[in] name Borrowed runtime name, or `NULL` for an empty name.
/// @return Newly appended zero-based shape index, or `-1` for an invalid
///         handle/name, count overflow, size failure, or allocation failure.
int64_t rt_morphtarget3d_add_shape(void *obj, rt_string name) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    char canonical_name[64];
    if (!mt)
        return -1;
    if (!morphtarget_copy_canonical_name(name, canonical_name))
        return -1;
    morphtarget_repair_shape_table(mt);
    if (mt->shape_count == INT32_MAX)
        return -1;
    if (!morphtarget_reserve_shapes(mt, mt->shape_count + 1)) {
        rt_trap("MorphTarget3D.AddShape: memory allocation failed");
        return -1;
    }

    vgfx3d_morph_shape_t *shape = &mt->shapes[mt->shape_count];
    memset(shape, 0, sizeof(vgfx3d_morph_shape_t));
    memcpy(shape->name, canonical_name, sizeof(shape->name));

    size_t delta_size;
    if (!morphtarget_vertex_delta_bytes(mt->vertex_count, &delta_size))
        return -1;
    shape->pos_deltas = (float *)calloc(1, delta_size);
    if (!shape->pos_deltas) {
        rt_trap("MorphTarget3D.AddShape: delta allocation failed");
        return -1;
    }
    shape->owned_pos_deltas = shape->pos_deltas;
    /* Normal deltas allocated on first use (SetNormalDelta) */

    morphtarget_touch_payload(mt);
    mt->initialized_shape_count = mt->shape_count + 1;
    return mt->shape_count++;
}

/// @brief Set the position delta for a single vertex of a single shape.
/// Out-of-range shape or vertex indices are silent no-ops; touches the payload
/// generation so GPU caches re-upload on the next draw.
/// @param[in,out] obj MorphTarget3D to modify.
/// @param[in] shape Zero-based shape index.
/// @param[in] vertex Zero-based vertex index.
/// @param[in] dx Position-X delta; invalid/out-of-float values become zero.
/// @param[in] dy Position-Y delta; invalid/out-of-float values become zero.
/// @param[in] dz Position-Z delta; invalid/out-of-float values become zero.
void rt_morphtarget3d_set_delta(
    void *obj, int64_t shape, int64_t vertex, double dx, double dy, double dz) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return;
    if (shape < 0 || shape >= morphtarget_safe_shape_count(mt))
        return;
    if (vertex < 0 || vertex >= mt->vertex_count)
        return;

    float candidate[3] = {morphtarget_sanitize_delta(dx),
                          morphtarget_sanitize_delta(dy),
                          morphtarget_sanitize_delta(dz)};
    vgfx3d_morph_shape_t *shape_record = &mt->shapes[shape];
    morphtarget_repair_shape(shape_record);
    float *pd = shape_record->pos_deltas;
    if (!pd)
        return;
    if (pd[vertex * 3 + 0] == candidate[0] && pd[vertex * 3 + 1] == candidate[1] &&
        pd[vertex * 3 + 2] == candidate[2])
        return;
    pd[vertex * 3 + 0] = candidate[0];
    pd[vertex * 3 + 1] = candidate[1];
    pd[vertex * 3 + 2] = candidate[2];
    morphtarget_touch_payload(mt);
}

/// @brief Set the normal delta for a single vertex of a single shape.
/// Lazy-allocates the per-shape normal delta array on first use to keep
/// position-only blendshapes memory-efficient. Triggers post-morph re-normalization.
/// @param[in,out] obj MorphTarget3D to modify.
/// @param[in] shape Zero-based shape index.
/// @param[in] vertex Zero-based vertex index.
/// @param[in] dx Normal-X delta; invalid/out-of-float values become zero.
/// @param[in] dy Normal-Y delta; invalid/out-of-float values become zero.
/// @param[in] dz Normal-Z delta; invalid/out-of-float values become zero.
void rt_morphtarget3d_set_normal_delta(
    void *obj, int64_t shape, int64_t vertex, double dx, double dy, double dz) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return;
    if (shape < 0 || shape >= morphtarget_safe_shape_count(mt))
        return;
    if (vertex < 0 || vertex >= mt->vertex_count)
        return;

    float candidate[3] = {morphtarget_sanitize_delta(dx),
                          morphtarget_sanitize_delta(dy),
                          morphtarget_sanitize_delta(dz)};
    vgfx3d_morph_shape_t *shape_record = &mt->shapes[shape];
    morphtarget_repair_shape(shape_record);
    /* A zero edit against the implicit sparse channel changes no payload and
     * must not allocate a vertex-sized array or force a normal payload. */
    if (!shape_record->nrm_deltas && candidate[0] == 0.0f && candidate[1] == 0.0f &&
        candidate[2] == 0.0f)
        return;
    /* Lazy-allocate normal deltas on first use */
    if (!shape_record->nrm_deltas) {
        size_t sz;
        float *allocated;
        if (!morphtarget_vertex_delta_bytes(mt->vertex_count, &sz))
            return;
        allocated = (float *)calloc(1, sz);
        if (!allocated) {
            rt_trap("MorphTarget3D.SetNormalDelta: delta allocation failed");
            return;
        }
        shape_record->owned_nrm_deltas = allocated;
        shape_record->nrm_deltas = allocated;
    }

    float *nd = shape_record->nrm_deltas;
    if (nd[vertex * 3 + 0] == candidate[0] && nd[vertex * 3 + 1] == candidate[1] &&
        nd[vertex * 3 + 2] == candidate[2])
        return;
    nd[vertex * 3 + 0] = candidate[0];
    nd[vertex * 3 + 1] = candidate[1];
    nd[vertex * 3 + 2] = candidate[2];
    morphtarget_touch_payload(mt);
}

/// @brief Set the tangent delta for a single vertex of a single shape.
/// Lazy-allocates tangent deltas on first use. Tangent handedness (w) is not
/// morphed; CPU fallback preserves the base tangent sign and renormalizes xyz.
/// @param[in,out] obj MorphTarget3D to modify.
/// @param[in] shape Zero-based shape index.
/// @param[in] vertex Zero-based vertex index.
/// @param[in] dx Tangent-X delta; invalid/out-of-float values become zero.
/// @param[in] dy Tangent-Y delta; invalid/out-of-float values become zero.
/// @param[in] dz Tangent-Z delta; invalid/out-of-float values become zero.
void rt_morphtarget3d_set_tangent_delta(
    void *obj, int64_t shape, int64_t vertex, double dx, double dy, double dz) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return;
    if (shape < 0 || shape >= morphtarget_safe_shape_count(mt))
        return;
    if (vertex < 0 || vertex >= mt->vertex_count)
        return;

    float candidate[3] = {morphtarget_sanitize_delta(dx),
                          morphtarget_sanitize_delta(dy),
                          morphtarget_sanitize_delta(dz)};
    vgfx3d_morph_shape_t *shape_record = &mt->shapes[shape];
    morphtarget_repair_shape(shape_record);
    if (!shape_record->tan_deltas && candidate[0] == 0.0f && candidate[1] == 0.0f &&
        candidate[2] == 0.0f)
        return;
    if (!shape_record->tan_deltas) {
        size_t sz;
        float *allocated;
        if (!morphtarget_vertex_delta_bytes(mt->vertex_count, &sz))
            return;
        allocated = (float *)calloc(1, sz);
        if (!allocated) {
            rt_trap("MorphTarget3D.SetTangentDelta: delta allocation failed");
            return;
        }
        shape_record->owned_tan_deltas = allocated;
        shape_record->tan_deltas = allocated;
    }

    float *td = shape_record->tan_deltas;
    if (td[vertex * 3 + 0] == candidate[0] && td[vertex * 3 + 1] == candidate[1] &&
        td[vertex * 3 + 2] == candidate[2])
        return;
    td[vertex * 3 + 0] = candidate[0];
    td[vertex * 3 + 1] = candidate[1];
    td[vertex * 3 + 2] = candidate[2];
    morphtarget_touch_payload(mt);
}

/*==========================================================================
 * Weight control
 *=========================================================================*/

/// @brief Set the blend weight for a shape by index (0.0 = off, 1.0 = full).
///
/// Clamped to [-1, 1]. Values outside that range silently over-extruded
/// vertices past the target mesh and produced z-fighting / self-intersection
/// in extreme cases. Negative weights are kept (they invert the morph delta)
/// but bounded so the combined deformation stays well-defined.
/// @param[in,out] obj MorphTarget3D to configure.
/// @param[in] shape Zero-based shape index.
/// @param[in] weight Requested signed contribution; non-finite input becomes
///                   zero.
void rt_morphtarget3d_set_weight(void *obj, int64_t shape, double weight) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return;
    if (shape < 0 || shape >= morphtarget_safe_shape_count(mt))
        return;
    mt->weights[shape] = morphtarget_sanitize_weight(weight);
}

/// @brief Get the current blend weight for a shape by index.
/// @details Sanitizes the stored lane in place before returning it.
/// @param[in,out] obj MorphTarget3D to inspect.
/// @param[in] shape Zero-based shape index.
/// @return Finite weight in `[-1,1]`, or zero for invalid input.
double rt_morphtarget3d_get_weight(void *obj, int64_t shape) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return 0.0;
    if (shape < 0 || shape >= morphtarget_safe_shape_count(mt))
        return 0.0;
    if (!mt->weights)
        return 0.0;
    mt->weights[shape] = morphtarget_sanitize_weight(mt->weights[shape]);
    return mt->weights[shape];
}

/// @brief Set the blend weight for a shape by its name (string lookup).
/// @details Facial rigs drive the same few shapes by name every frame, so the last hit is
///          memoized; the memo is re-verified by name before use, so shape-list mutations
///          can never make it resolve to the wrong shape.
/// @param[in,out] obj MorphTarget3D to configure.
/// @param[in] name Borrowed nonempty runtime name, canonicalized to 63 bytes.
/// @param[in] weight Requested signed contribution, sanitized as for indexed
///                   assignment.
void rt_morphtarget3d_set_weight_by_name(void *obj, rt_string name, double weight) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt || !name)
        return;
    char target[64];
    if (!morphtarget_copy_canonical_name(name, target) || target[0] == '\0')
        return;
    int32_t count = morphtarget_safe_shape_count(mt);
    int32_t memo = mt->name_lookup_memo;
    if (memo >= 0 && memo < count && strcmp(mt->shapes[memo].name, target) == 0) {
        rt_morphtarget3d_set_weight(mt, memo, weight);
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(mt->shapes[i].name, target) == 0) {
            mt->name_lookup_memo = i;
            rt_morphtarget3d_set_weight(mt, i, weight);
            return;
        }
    }
}

/// @brief Get the number of registered blend shapes.
/// @param[in] obj MorphTarget3D to inspect.
/// @return Sanitized readable shape count, or zero for an invalid handle.
int64_t rt_morphtarget3d_get_shape_count(void *obj) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    return mt ? morphtarget_safe_shape_count(mt) : 0;
}

/// @brief Expose one validated shape through the narrow persistence-only borrowed view.
/// @details Clears @p out_view before validation. All returned pointers remain
///          owned by the morph target.
/// @param[in] obj MorphTarget3D to inspect.
/// @param[in] shape_index Zero-based shape index.
/// @param[out] out_view Writable view record.
/// @return `1` when a positive-vertex shape with position deltas was exposed;
///         otherwise `0`.
int8_t rt_morphtarget3d_get_shape_view_internal(void *obj,
                                                int64_t shape_index,
                                                rt_morphtarget3d_shape_view_internal *out_view) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    int32_t shape_count;
    if (out_view)
        memset(out_view, 0, sizeof(*out_view));
    if (!mt || !out_view || shape_index < 0)
        return 0;
    shape_count = morphtarget_safe_shape_count(mt);
    if (shape_index >= shape_count)
        return 0;
    if (!morphtarget_repair_shape(&mt->shapes[shape_index]))
        return 0;
    out_view->name = mt->shapes[shape_index].name;
    out_view->position_deltas = mt->shapes[shape_index].pos_deltas;
    out_view->normal_deltas = mt->shapes[shape_index].nrm_deltas;
    out_view->tangent_deltas = mt->shapes[shape_index].tan_deltas;
    out_view->weight = mt->weights ? morphtarget_sanitize_weight(mt->weights[shape_index]) : 0.0;
    out_view->vertex_count = mt->vertex_count;
    return out_view->position_deltas && out_view->vertex_count > 0 ? 1 : 0;
}

/// @brief Validate and copy a complete persistence shape into a morph container.
/// @details Requires an exact positive vertex-count match, a name of at most
///          63 bytes, finite weight, and finite required position plus optional
///          normal/tangent lanes. Copies all supplied bytes before publishing
///          the new shape.
/// @param[in,out] obj Destination MorphTarget3D.
/// @param[in] view Borrowed complete source-shape view.
/// @return `1` after atomic publication; `0` for validation, capacity, size,
///         or allocation failure.
int8_t rt_morphtarget3d_append_shape_internal(void *obj,
                                              const rt_morphtarget3d_shape_view_internal *view) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    vgfx3d_morph_shape_t *shape;
    float *position_copy = NULL;
    float *normal_copy = NULL;
    float *tangent_copy = NULL;
    size_t delta_size;
    size_t value_count;
    size_t name_length;
    if (!mt || !morphtarget_repair_storage(mt) || !view || !view->name)
        return 0;
    name_length = morphtarget_bounded_cstr_length(view->name, 64u);
    if (!view->position_deltas || view->vertex_count <= 0 ||
        view->vertex_count != mt->vertex_count || name_length > 63u || !isfinite(view->weight) ||
        !morphtarget_vertex_delta_bytes(mt->vertex_count, &delta_size))
        return 0;
    value_count = (size_t)mt->vertex_count * 3u;
    for (size_t i = 0; i < value_count; ++i) {
        if (!isfinite(view->position_deltas[i]) ||
            (view->normal_deltas && !isfinite(view->normal_deltas[i])) ||
            (view->tangent_deltas && !isfinite(view->tangent_deltas[i])))
            return 0;
    }
    position_copy = (float *)malloc(delta_size);
    if (!position_copy)
        goto fail;
    memcpy(position_copy, view->position_deltas, delta_size);
    if (view->normal_deltas) {
        normal_copy = (float *)malloc(delta_size);
        if (!normal_copy)
            goto fail;
        memcpy(normal_copy, view->normal_deltas, delta_size);
    }
    if (view->tangent_deltas) {
        tangent_copy = (float *)malloc(delta_size);
        if (!tangent_copy)
            goto fail;
        memcpy(tangent_copy, view->tangent_deltas, delta_size);
    }
    morphtarget_repair_shape_table(mt);
    if (mt->shape_count == INT32_MAX || !morphtarget_reserve_shapes(mt, mt->shape_count + 1))
        goto fail;
    shape = &mt->shapes[mt->shape_count];
    memset(shape, 0, sizeof(*shape));
    memcpy(shape->name, view->name, name_length);
    shape->name[name_length] = '\0';
    shape->pos_deltas = position_copy;
    shape->nrm_deltas = normal_copy;
    shape->tan_deltas = tangent_copy;
    shape->owned_pos_deltas = position_copy;
    shape->owned_nrm_deltas = normal_copy;
    shape->owned_tan_deltas = tangent_copy;
    position_copy = NULL;
    normal_copy = NULL;
    tangent_copy = NULL;
    mt->weights[mt->shape_count] = morphtarget_sanitize_weight(view->weight);
    mt->prev_weights[mt->shape_count] = mt->weights[mt->shape_count];
    mt->motion_weight_snapshot[mt->shape_count] = mt->weights[mt->shape_count];
    mt->shape_count++;
    mt->initialized_shape_count = mt->shape_count;
    morphtarget_touch_payload(mt);
    return 1;

fail:
    free(position_copy);
    free(normal_copy);
    free(tangent_copy);
    return 0;
}

/// @brief Borrow the contiguous packed position-delta payload for GPU upload.
/// Layout: shape-major, [shape][vertex][xyz]. Rebuilds on demand if dirty.
/// Returns NULL if the morph target is empty or rebuild fails (OOM).
/// @param[in,out] obj MorphTarget3D whose lazy payload may be rebuilt.
/// @return Borrowed shape-major array of `shape_count * vertex_count * 3`
///         floats, or `NULL`.
const float *rt_morphtarget3d_get_packed_deltas(void *obj) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return NULL;
    if (!morphtarget_rebuild_packed_payload(mt))
        return NULL;
    return mt->packed_pos_deltas;
}

/// @brief Largest authored position delta length across all shapes.
/// @details The unweighted maximum is cached against payload generation and
///          recomputed after any delta mutation.
/// @param[in,out] obj MorphTarget3D to inspect.
/// @return Largest finite Euclidean position-delta length, or zero.
double rt_morphtarget3d_get_max_position_delta(void *obj) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    double max_len = 0.0;
    if (!mt || !morphtarget_repair_storage(mt) || mt->vertex_count <= 0)
        return 0.0;
    if (mt->max_delta_generation == mt->payload_generation &&
        isfinite(mt->max_position_delta_cache) && mt->max_position_delta_cache >= 0.0)
        return mt->max_position_delta_cache;
    for (int32_t s = 0, count = morphtarget_safe_shape_count(mt); s < count; s++) {
        const float *deltas = mt->shapes[s].pos_deltas;
        if (!deltas)
            continue;
        for (int32_t v = 0; v < mt->vertex_count; v++) {
            const float *d = deltas + (size_t)v * 3u;
            double len = morphtarget_delta_length_or_zero(d);
            if (len > max_len)
                max_len = len;
        }
    }
    mt->max_position_delta_cache = max_len;
    mt->max_delta_generation = mt->payload_generation;
    return max_len;
}

/// @brief Borrow the packed normal-delta payload for GPU upload, or NULL when no
/// shape has normal deltas. Same layout as `_get_packed_deltas`.
/// @param[in,out] obj MorphTarget3D whose lazy payload may be rebuilt.
/// @return Borrowed shape-major normal array, or `NULL` when absent, invalid,
///         empty, or rebuilding fails.
const float *rt_morphtarget3d_get_packed_normal_deltas(void *obj) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return NULL;
    if (!morphtarget_rebuild_packed_payload(mt))
        return NULL;
    return mt->packed_nrm_deltas;
}

/// @brief Return 1 if any shape in the morph target has packed tangent deltas, 0 otherwise.
/// @details Used by the renderer to decide whether to upload and apply tangent-space
///          morph deltas, which incur an additional GPU pass.
/// @param[in] obj MorphTarget3D to inspect.
/// @return `1` when any safe shape owns tangent deltas; otherwise `0`.
int64_t rt_morphtarget3d_has_tangent_deltas(void *obj) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt)
        return 0;
    for (int32_t i = 0, count = morphtarget_safe_shape_count(mt); i < count; i++) {
        if (mt->shapes[i].tan_deltas)
            return 1;
    }
    return 0;
}

/// @brief Monotonic counter that bumps whenever any delta changes.
/// GPU caches compare against the previous value to detect when re-upload is required.
/// @details Wrap skips zero so valid objects always expose a nonzero generation.
/// @param[in,out] obj MorphTarget3D to inspect.
/// @return Nonzero payload generation, or zero for an invalid handle.
uint64_t rt_morphtarget3d_get_payload_generation(void *obj) {
    rt_morphtarget3d *mt = morphtarget_checked(obj);
    if (!mt || !morphtarget_repair_storage(mt))
        return 0;
    return mt->payload_generation;
}

/// @brief Advance the previous-weight history by one frame for motion vectors.
/// @details Keyed on `frame_serial` so multiple draws within the same frame
///   don't rotate history more than once: the first draw of a new frame
///   copies the snapshot from the previous frame into `prev_weights` (so
///   motion-vector shaders can see the delta) and then snapshots the
///   current weights for next frame. `has_prev_weights` stays 0 on the
///   first-ever frame so motion shaders don't blend against zero noise.
/// @param[in,out] mt Morph target whose current and history arrays to sanitize
///                   and rotate.
/// @param[in] frame_serial Canvas frame identifier; repeat values do not
///                         advance history again.
/// @return The previous-frame weights for motion-vector use, or NULL when
///   no history exists yet.
static const float *morphtarget_prepare_prev_weights(rt_morphtarget3d *mt, int64_t frame_serial) {
    if (!mt)
        return NULL;
    morphtarget_sanitize_weights(mt);
    if (!mt->motion_frame_initialized || mt->last_motion_frame != frame_serial) {
        int32_t shape_count = morphtarget_safe_shape_count(mt);
        size_t weight_bytes = (size_t)shape_count * sizeof(float);
        if (mt->motion_frame_initialized && weight_bytes > 0) {
            memcpy(mt->prev_weights, mt->motion_weight_snapshot, weight_bytes);
            mt->has_prev_weights = 1;
        }
        if (weight_bytes > 0)
            memcpy(mt->motion_weight_snapshot, mt->weights, weight_bytes);
        mt->last_motion_frame = frame_serial;
        mt->motion_frame_initialized = 1;
    }
    return mt->has_prev_weights ? mt->prev_weights : NULL;
}

/*==========================================================================
 * Mesh integration
 *=========================================================================*/

/// @brief Drop one retained object ref and clear the slot.
/// @param[in,out] slot Address of a retained runtime-object slot.
static void mesh3d_release_ref(void **slot) {
    rt_g3d_ref_slot_release(slot);
}

/// @brief Release a retained MorphTarget3D slot only if it still points at MorphTarget3D.
/// @details A wrong-class non-null value is cleared as an unowned corrupt slot.
/// @param[in,out] slot Address of the mesh's retained morph-target slot.
static void mesh3d_release_morph_slot(void **slot) {
    if (!slot || !*slot)
        return;
    if (!rt_g3d_has_class(*slot, RT_G3D_MORPHTARGET3D_CLASS_ID)) {
        rt_g3d_ref_slot_clear_unowned(slot);
        return;
    }
    mesh3d_release_ref(slot);
}

/// @brief Bind a MorphTarget3D to a Mesh3D so subsequent draws apply blendshapes.
/// Vertex counts must match exactly; mismatches are silently rejected. Pass NULL
/// in @p morph_targets to detach.
/// @details A successful replacement retains the new container, releases the
///          old one, and marks mesh geometry changed. Rebinding the same object
///          is a no-op.
/// @param[in,out] mesh Mesh3D whose optional morph reference to update.
/// @param[in] morph_targets Borrowed equal-vertex-count MorphTarget3D, or
///                          `NULL` to detach.
void rt_mesh3d_set_morph_targets(void *mesh, void *morph_targets) {
    rt_mesh3d *m = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    if (!m)
        return;
    if (m->morph_targets_ref &&
        !rt_g3d_has_class(m->morph_targets_ref, RT_G3D_MORPHTARGET3D_CLASS_ID))
        mesh3d_release_morph_slot(&m->morph_targets_ref);
    if (!morph_targets) {
        if (!m->morph_targets_ref)
            return;
        mesh3d_release_morph_slot(&m->morph_targets_ref);
        rt_mesh3d_touch_geometry(m);
        return;
    }
    rt_morphtarget3d *mt = morphtarget_checked(morph_targets);
    if (!mt)
        return;
    if (!morphtarget_repair_storage(mt))
        return;
    if (mt->vertex_count <= 0 || (int32_t)m->vertex_count != mt->vertex_count)
        return;
    if (m->morph_targets_ref == morph_targets)
        return;
    rt_obj_retain_maybe(morph_targets);
    mesh3d_release_morph_slot(&m->morph_targets_ref);
    m->morph_targets_ref = morph_targets;
    rt_mesh3d_touch_geometry(m);
}

/*==========================================================================
 * CPU morph application + drawing
 *=========================================================================*/

/// @brief True when any shape has a meaningful finite weight and position-delta payload.
/// @param[in] mt Morph target to inspect.
/// @return Nonzero when a safe shape has position storage and
///         `abs(weight) >= 1e-6`; otherwise zero.
static int morphtarget_has_active_position_deltas(rt_morphtarget3d *mt) {
    int32_t shape_count = morphtarget_safe_shape_count(mt);
    if (!mt || shape_count <= 0 || !mt->weights)
        return 0;
    for (int32_t s = 0; s < shape_count; s++) {
        float w = mt->weights[s];
        if (mt->shapes[s].pos_deltas && isfinite(w) && fabsf(w) >= 1e-6f)
            return 1;
    }
    return 0;
}

/// @brief Add one weighted morph lane without overflowing the float destination.
/// @param base Current finite or corrupt destination lane.
/// @param weight Sanitized signed morph weight.
/// @param delta Authored lane, sanitized by this helper.
/// @return Finite accumulated lane saturated at the float range.
static float morphtarget_accumulate_lane(float base, float weight, float delta) {
    double value = (isfinite(base) ? (double)base : 0.0) +
                   (double)weight * (double)morphtarget_sanitize_delta(delta);
    if (!isfinite(value))
        return value < 0.0 ? -FLT_MAX : FLT_MAX;
    if (value > FLT_MAX)
        return FLT_MAX;
    if (value < -FLT_MAX)
        return -FLT_MAX;
    return (float)value;
}

/// @brief Accumulate weighted morph-target position/normal/tangent deltas into @p morphed,
///        re-normalize affected directions, and replace any non-finite position with the base.
/// @details Pure CPU blend extracted from morphtarget_draw_mesh_matrix; weights are pre-sanitized.
/// @param[in] mt Morph target providing aligned shape channels and weights.
/// @param[in] base_vertices Base vertex array the blend falls back to for
///                          non-finite results and direction re-normalization.
/// @param[in] vertex_count Number of vertices in both arrays.
/// @param[in] shape_count Safe number of shapes to accumulate.
/// @param[in,out] morphed Writable base-initialized vertex array.
static void morphtarget_accumulate_morphed_vertices(rt_morphtarget3d *mt,
                                                    const vgfx3d_vertex_t *base_vertices,
                                                    uint32_t vertex_count,
                                                    int32_t shape_count,
                                                    vgfx3d_vertex_t *morphed) {
    int has_normal_deltas = 0;
    int has_tangent_deltas = 0;
    for (int32_t s = 0; s < shape_count; s++) {
        /* Weights were already sanitized in place by morphtarget_sanitize_weights()
         * above, so read the stored value directly instead of re-clamping it. */
        float w = mt->weights[s];
        if (!isfinite(w) || fabsf(w) < 1e-6f)
            continue;

        const float *pd = mt->shapes[s].pos_deltas;
        const float *nd = mt->shapes[s].nrm_deltas;
        const float *td = mt->shapes[s].tan_deltas;
        if (!pd)
            continue;

        for (uint32_t v = 0; v < vertex_count; v++) {
            size_t base = (size_t)v * 3u;
            morphed[v].pos[0] = morphtarget_accumulate_lane(morphed[v].pos[0], w, pd[base + 0]);
            morphed[v].pos[1] = morphtarget_accumulate_lane(morphed[v].pos[1], w, pd[base + 1]);
            morphed[v].pos[2] = morphtarget_accumulate_lane(morphed[v].pos[2], w, pd[base + 2]);

            if (nd) {
                morphed[v].normal[0] =
                    morphtarget_accumulate_lane(morphed[v].normal[0], w, nd[base + 0]);
                morphed[v].normal[1] =
                    morphtarget_accumulate_lane(morphed[v].normal[1], w, nd[base + 1]);
                morphed[v].normal[2] =
                    morphtarget_accumulate_lane(morphed[v].normal[2], w, nd[base + 2]);
                has_normal_deltas = 1;
            }
            if (td) {
                morphed[v].tangent[0] =
                    morphtarget_accumulate_lane(morphed[v].tangent[0], w, td[base + 0]);
                morphed[v].tangent[1] =
                    morphtarget_accumulate_lane(morphed[v].tangent[1], w, td[base + 1]);
                morphed[v].tangent[2] =
                    morphtarget_accumulate_lane(morphed[v].tangent[2], w, td[base + 2]);
                has_tangent_deltas = 1;
            }
        }
    }

    /* Re-normalize normals if any shape had normal deltas */
    if (has_normal_deltas) {
        for (uint32_t v = 0; v < vertex_count; v++) {
            float *n = morphed[v].normal;
            morphtarget_normalize3_or_copy(n, base_vertices[v].normal);
        }
    }

    if (has_tangent_deltas) {
        for (uint32_t v = 0; v < vertex_count; v++) {
            float *t = morphed[v].tangent;
            morphtarget_normalize3_or_copy(t, base_vertices[v].tangent);
        }
    }

    for (uint32_t v = 0; v < vertex_count; v++) {
        if (!isfinite(morphed[v].pos[0]) || !isfinite(morphed[v].pos[1]) ||
            !isfinite(morphed[v].pos[2])) {
            for (int lane = 0; lane < 3; ++lane)
                morphed[v].pos[lane] =
                    isfinite(base_vertices[v].pos[lane]) ? base_vertices[v].pos[lane] : 0.0f;
        }
    }
}

/// @brief Blend a container's active shapes over @p src_vertices into @p dst_vertices.
/// @details CPU pre-pass for deformation paths that must compose exactly like
///          the GPU vertex shader: morph the bind-space vertices FIRST, then
///          skin. The CPU-skinning fallback calls this before palette skinning;
///          without it, bind-space deltas were applied to already-posed
///          vertices and pointed the wrong way once a bone left its bind
///          orientation. Both arrays are `vgfx3d_vertex_t[vertex_count]`
///          (passed as `void *` to keep this header vgfx3d-free). Weights are
///          sanitized in place; the copy happens only when a blend will run.
/// @param[in] morph MorphTarget3D whose vertex count must equal @p vertex_count.
/// @param[in] src_vertices Base bind-space vertex array.
/// @param[out] dst_vertices Destination array receiving base plus weighted deltas.
/// @return One when @p dst_vertices holds a blended copy; zero (destination
///         untouched) for invalid handles, a count mismatch, or no shape with
///         an effective weight — callers then skin @p src_vertices directly.
int8_t rt_morphtarget3d_blend_vertices_internal(void *morph,
                                                const void *src_vertices,
                                                void *dst_vertices,
                                                uint32_t vertex_count) {
    rt_morphtarget3d *mt = morphtarget_checked(morph);
    const vgfx3d_vertex_t *src = (const vgfx3d_vertex_t *)src_vertices;
    vgfx3d_vertex_t *dst = (vgfx3d_vertex_t *)dst_vertices;
    if (!mt || !src || !dst || vertex_count == 0)
        return 0;
    morphtarget_repair_shape_table(mt);
    morphtarget_sanitize_weights(mt);
    int32_t shape_count = morphtarget_safe_shape_count(mt);
    if ((uint32_t)mt->vertex_count != vertex_count)
        return 0;
    if (!morphtarget_has_active_position_deltas(mt))
        return 0;
    memcpy(dst, src, (size_t)vertex_count * sizeof(vgfx3d_vertex_t));
    morphtarget_accumulate_morphed_vertices(mt, src, vertex_count, shape_count, dst);
    return 1;
}

/// @brief Submit a mesh draw with morph-target blend applied on GPU or CPU.
/// @details Picks the evaluation path by asking the backend
///   (`vgfx3d_backend_prefers_gpu_morph`). GPU path: builds a shallow stack
///   copy of the mesh with its morph fields pointed at the packed payload for
///   this draw and submits that copy via the normal `draw_mesh_matrix_keyed`,
///   avoiding any mutation of the persistent mesh state. Because the copy
///   aliases the original mesh's vertex/index buffers, the original mesh (and
///   the morph target) are retained as frame temp objects so they outlive the
///   deferred draw. CPU path: allocates a scratch vertex buffer,
///   accumulates weighted position plus optional normal/tangent deltas per
///   vertex, re-normalizes affected directions when any shape contributed them,
///   and tracks the buffer for end-of-frame cleanup. Small weights
///   (|w| < 1e-6) are skipped to avoid unnecessary fp math when a shape
///   is effectively dormant.
/// @param[in,out] canvas Canvas3D or supported stack canvas receiving the
///                       deferred draw.
/// @param[in] mesh Mesh3D handle or internal stack mesh.
/// @param[in] model_matrix Borrowed row-major 4-by-4 double transform.
/// @param[in] material Borrowed material handle required by draw submission.
/// @param[in] motion_key Optional borrowed stable identity for temporal motion.
/// @param[in] morph_targets Borrowed MorphTarget3D matching mesh vertex count.
/// @param[in] local_bounds_min Optional borrowed three-float local minimum.
/// @param[in] local_bounds_max Optional borrowed three-float local maximum.
/// @param[in] conservative_bounds Requested conservative-bounds flag used by
///                                the no-active-morph fast path.
/// @param[in] disable_occlusion Requested occlusion flag used by the
///                              no-active-morph fast path; active morph paths
///                              force conservative bounds and disable occlusion.
static void morphtarget_draw_mesh_matrix(void *canvas,
                                         void *mesh,
                                         const double *model_matrix,
                                         void *material,
                                         const void *motion_key,
                                         void *morph_targets,
                                         const float *local_bounds_min,
                                         const float *local_bounds_max,
                                         int8_t conservative_bounds,
                                         int8_t disable_occlusion) {
    if (!canvas || !mesh || !model_matrix || !material || !morph_targets)
        return;

    rt_canvas3d *c = rt_canvas3d_checked_or_stack(canvas);
    rt_mesh3d *m = (rt_mesh3d *)rt_g3d_checked_or_null(mesh, RT_G3D_MESH3D_CLASS_ID);
    rt_morphtarget3d *mt = morphtarget_checked(morph_targets);
    const float *prev_weights;
    if (!m && !rt_heap_is_payload(mesh))
        m = (rt_mesh3d *)mesh;
    if (!c || !m || !mt)
        return;
    morphtarget_repair_shape_table(mt);
    morphtarget_sanitize_weights(mt);
    int32_t shape_count = morphtarget_safe_shape_count(mt);

    if (!m->resident)
        return;
    if (m->vertex_count == 0)
        return;
    if (m->vertex_count != (uint32_t)mt->vertex_count)
        return;
    if (!morphtarget_has_active_position_deltas(mt)) {
        rt_mesh3d tmp = *m;
        if (rt_heap_is_payload(mesh) && !rt_canvas3d_add_temp_object(canvas, mesh))
            return;
        /* Resource rejection is not a draw: commit temporal history only after
         * this path has secured every lifetime dependency it can fail to acquire. */
        (void)morphtarget_prepare_prev_weights(mt, rt_canvas3d_get_frame_serial(canvas));
        tmp.morph_targets_ref = NULL;
        tmp.morph_deltas = NULL;
        tmp.morph_normal_deltas = NULL;
        tmp.morph_weights = NULL;
        tmp.prev_morph_weights = NULL;
        tmp.morph_shape_count = 0;
        rt_canvas3d_draw_mesh_matrix_keyed_bounds(canvas,
                                                  &tmp,
                                                  model_matrix,
                                                  material,
                                                  motion_key,
                                                  NULL,
                                                  NULL,
                                                  local_bounds_min,
                                                  local_bounds_max,
                                                  conservative_bounds,
                                                  disable_occlusion,
                                                  0.0f);
        return;
    }

    if (c->backend && !rt_morphtarget3d_has_tangent_deltas(mt) &&
        vgfx3d_backend_prefers_gpu_morph(c->backend->name, shape_count)) {
        const float *packed_deltas = rt_morphtarget3d_get_packed_deltas(mt);
        const float *packed_normal_deltas = rt_morphtarget3d_get_packed_normal_deltas(mt);
        int morph_was_tracked;
        rt_mesh3d tmp;
        if (shape_count > 0 && !packed_deltas)
            return;
        morph_was_tracked = canvas3d_temp_object_set_contains(c, mt);
        if (!rt_canvas3d_add_temp_object(canvas, mt))
            return;
        /* The stack mesh copy below aliases the original mesh's vertex/index
         * buffers, so the original mesh object must outlive the deferred draw.
         * Passing the copy to the keyed draw skips that path's payload
         * retention, so retain the original mesh explicitly here. */
        if (rt_heap_is_payload(mesh) && !rt_canvas3d_add_temp_object(canvas, mesh)) {
            if (!morph_was_tracked)
                canvas3d_release_tracked_temp_object(c, mt);
            return;
        }

        prev_weights = morphtarget_prepare_prev_weights(mt, rt_canvas3d_get_frame_serial(canvas));
        tmp = *m;
        tmp.morph_targets_ref = morph_targets;
        tmp.morph_deltas = packed_deltas;
        tmp.morph_normal_deltas = packed_normal_deltas;
        tmp.morph_weights = mt->weights;
        tmp.prev_morph_weights = prev_weights;
        tmp.morph_shape_count = shape_count;
        rt_canvas3d_draw_mesh_matrix_keyed_bounds(canvas,
                                                  &tmp,
                                                  model_matrix,
                                                  material,
                                                  motion_key,
                                                  NULL,
                                                  prev_weights,
                                                  local_bounds_min,
                                                  local_bounds_max,
                                                  1,
                                                  1,
                                                  0.0f);
        return;
    }

    /* Allocate morphed vertex buffer */
    if (!rt_alloc_count_ok(m->vertex_count, sizeof(vgfx3d_vertex_t)))
        return;
    vgfx3d_vertex_t *morphed =
        (vgfx3d_vertex_t *)malloc((size_t)m->vertex_count * sizeof(vgfx3d_vertex_t));
    if (!morphed)
        return;

    /* Start with base mesh */
    memcpy(morphed, m->vertices, (size_t)m->vertex_count * sizeof(vgfx3d_vertex_t));

    /* Accumulate weighted deltas, re-normalize, and sanitize non-finite positions (CPU morph). */
    morphtarget_accumulate_morphed_vertices(mt, m->vertices, m->vertex_count, shape_count, morphed);

    int mesh_was_tracked = 0;
    if (rt_heap_is_payload(mesh)) {
        mesh_was_tracked = canvas3d_temp_object_set_contains(c, mesh);
        if (!rt_canvas3d_add_temp_object(canvas, mesh)) {
            free(morphed);
            return;
        }
    }

    /* Register buffer for end-of-frame cleanup (avoids use-after-free
     * with deferred draw queue). */
    if (!rt_canvas3d_add_temp_buffer(canvas, morphed)) {
        if (rt_heap_is_payload(mesh) && !mesh_was_tracked)
            canvas3d_release_tracked_temp_object(c, mesh);
        free(morphed);
        return;
    }

    (void)morphtarget_prepare_prev_weights(mt, rt_canvas3d_get_frame_serial(canvas));

    /* Submit via normal draw pipeline */
    rt_mesh3d tmp = *m;
    tmp.vertices = morphed;
    tmp.positions64 = NULL;
    tmp.morph_targets_ref = NULL;
    tmp.morph_deltas = NULL;
    tmp.morph_normal_deltas = NULL;
    tmp.morph_weights = NULL;
    tmp.prev_morph_weights = NULL;
    tmp.morph_shape_count = 0;
    tmp.bounds_dirty = 1;
    rt_mesh3d_refresh_bounds(&tmp);
    rt_canvas3d_draw_mesh_matrix_keyed_bounds(canvas,
                                              &tmp,
                                              model_matrix,
                                              material,
                                              motion_key,
                                              NULL,
                                              NULL,
                                              local_bounds_min,
                                              local_bounds_max,
                                              1,
                                              1,
                                              0.0f);
}

/// @brief Draw a mesh with morph targets applied, using a raw 4×4 model matrix.
/// On GPU-capable backends (Metal/OpenGL/D3D11), uploads packed deltas + weights
/// and blends in the vertex shader. On other backends, applies CPU morph then
/// submits via the standard draw pipeline.
/// @param[in,out] canvas Canvas3D receiving the deferred draw.
/// @param[in] mesh Mesh3D whose vertex count must match the morph target.
/// @param[in] model_matrix Borrowed row-major 4-by-4 double transform.
/// @param[in] material Borrowed material handle.
/// @param[in] motion_key Optional borrowed temporal identity key.
/// @param[in] morph_targets Borrowed MorphTarget3D.
void rt_canvas3d_draw_mesh_matrix_morphed(void *canvas,
                                          void *mesh,
                                          const double *model_matrix,
                                          void *material,
                                          const void *motion_key,
                                          void *morph_targets) {
    morphtarget_draw_mesh_matrix(
        canvas, mesh, model_matrix, material, motion_key, morph_targets, NULL, NULL, 1, 1);
}

/// @brief Draw a morph-target mesh with caller-provided conservative bounds and occlusion flags.
/// @details Used by Scene3D when animation has already expanded local bounds. The public morphed
///          draw wrapper remains conservative by default; this internal variant prevents the
///          attached-morph fast path from discarding Scene3D's safer culling metadata.
/// @param[in,out] canvas Canvas3D receiving the deferred draw.
/// @param[in] mesh Mesh3D whose vertex count must match the morph target.
/// @param[in] model_matrix Borrowed row-major 4-by-4 double transform.
/// @param[in] material Borrowed material handle.
/// @param[in] motion_key Optional borrowed temporal identity key.
/// @param[in] morph_targets Borrowed MorphTarget3D.
/// @param[in] local_bounds_min Optional borrowed three-float local minimum.
/// @param[in] local_bounds_max Optional borrowed three-float local maximum.
/// @param[in] conservative_bounds Caller-computed conservative-bounds flag for
///                                the no-active-morph path.
/// @param[in] disable_occlusion Caller-computed occlusion-disable flag for the
///                              no-active-morph path.
void rt_canvas3d_draw_mesh_matrix_morphed_bounds(void *canvas,
                                                 void *mesh,
                                                 const double *model_matrix,
                                                 void *material,
                                                 const void *motion_key,
                                                 void *morph_targets,
                                                 const float *local_bounds_min,
                                                 const float *local_bounds_max,
                                                 int8_t conservative_bounds,
                                                 int8_t disable_occlusion) {
    morphtarget_draw_mesh_matrix(canvas,
                                 mesh,
                                 model_matrix,
                                 material,
                                 motion_key,
                                 morph_targets,
                                 local_bounds_min,
                                 local_bounds_max,
                                 conservative_bounds,
                                 disable_occlusion);
}

/// @brief Draw a morphed mesh using a Mat4 transform handle (convenience wrapper).
/// The transform doubles as the motion-vector key for temporal effects (TAA, motion blur).
/// @param[in,out] canvas Canvas3D receiving the deferred draw.
/// @param[in] mesh Mesh3D whose vertex count must match the morph target.
/// @param[in] transform Borrowed live Mat4 used for both model transform and
///                      temporal identity.
/// @param[in] material Borrowed material handle.
/// @param[in] morph_targets Borrowed MorphTarget3D.
void rt_canvas3d_draw_mesh_morphed(
    void *canvas, void *mesh, void *transform, void *material, void *morph_targets) {
    mat4_impl *transform_mat = morphtarget_mat4_checked(transform);
    if (!transform_mat)
        return;
    morphtarget_draw_mesh_matrix(
        canvas, mesh, transform_mat->m, material, transform, morph_targets, NULL, NULL, 1, 1);
}

#else
typedef int rt_graphics_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
