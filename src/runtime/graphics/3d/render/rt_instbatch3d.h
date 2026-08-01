//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_instbatch3d.h
// Purpose: Instanced rendering — draw N copies of one mesh with different
//   transforms in a single draw call. Software fallback loops individual draws.
//
// Key invariants:
//   - Mesh and material are retained by the batch while it is alive.
//   - Authoritative transforms are double[16*N], with contiguous float mirrors.
//   - Per-instance frustum culling at Canvas3D level before submission.
//
// Ownership/Lifetime:
//   - Returned batches are GC-managed and retain their mesh/material.
//   - Borrowed internal bridge pointers remain owned by the batch.
//
// Links: rt_canvas3d.h, vgfx3d_backend.h
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares retained InstanceBatch3D storage and Canvas3D submission APIs.
/// @details A batch binds one mesh/material pair to an ordered collection of
///   row-major Mat4 transforms, supporting mutation, motion-aware rendering,
///   per-instance culling, and optional per-instance animation palettes.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create an instanced-rendering batch bound to one mesh + material.
/// @param mesh Live Mesh3D retained for the batch lifetime.
/// @param material Live Material3D retained for the batch lifetime.
/// @return New GC-managed empty batch, or `NULL` for invalid handles or allocation failure.
void *rt_instbatch3d_new(void *mesh, void *material);

/// @brief Append an instance with the given Mat4 transform to the batch.
/// @param batch InstanceBatch3D receiver; invalid handles are ignored.
/// @param transform Live Mat4 copied and sanitized into batch-owned storage.
void rt_instbatch3d_add(void *batch, void *transform);

/// @brief Remove the instance at @p index via swap-remove (O(1), order not preserved).
/// @param batch InstanceBatch3D receiver; invalid handles are ignored.
/// @param index Zero-based instance index; out-of-range values are ignored.
void rt_instbatch3d_remove(void *batch, int64_t index);

/// @brief Replace the transform of the @p index-th instance.
/// @param batch InstanceBatch3D receiver; invalid handles are ignored.
/// @param index Zero-based instance index; out-of-range values are ignored.
/// @param transform Live Mat4 copied into authoritative and submit-mirror storage.
void rt_instbatch3d_set(void *batch, int64_t index, void *transform);

/// @brief Drop every instance from the batch.
/// @param batch InstanceBatch3D receiver; allocated capacity is retained for reuse.
void rt_instbatch3d_clear(void *batch);

/// @brief `InstanceBatch3D.GetTransform(index)` — read one instance back (ADR 0227).
/// @param batch InstanceBatch3D receiver.
/// @param index Zero-based instance index.
/// @return New Mat4 copy of the authoritative transform, or identity for
///         invalid handles or out-of-range indices.
void *rt_instbatch3d_get(void *batch, int64_t index);

/// @brief Number of instances currently in the batch.
/// @param batch InstanceBatch3D receiver.
/// @return Repaired nonnegative instance count, or zero for invalid/unrecoverable state.
int64_t rt_instbatch3d_count(void *batch);

/// @brief Queue the batch through the canvas instanced-rendering path.
/// @details Performs camera-relative conversion or per-instance culling and
///   once-per-frame double-precision motion-history capture before downstream
///   native or software submission. Camera-relative current and previous matrices
///   are culled and submitted in the same frame space.
/// @param canvas Canvas3D receiver with an active frame.
/// @param batch InstanceBatch3D receiver borrowed during queue construction.
void rt_canvas3d_draw_instanced(void *canvas, void *batch);

/// @brief Queue a skinned batch with one animator palette per instance.
/// @param canvas Canvas3D receiver with an active frame.
/// @param batch InstanceBatch3D supplying mesh, material, and transforms.
/// @param players Runtime sequence containing exactly one compatible AnimPlayer3D
///   or AnimController3D per instance, all with the same supported bone count.
void rt_canvas3d_draw_instanced_skinned(void *canvas, void *batch, void *players);

/// @brief Internal bridge: borrow the batch's retained mesh (NULL for invalid handles).
/// @param batch InstanceBatch3D receiver.
/// @return Borrowed live Mesh3D handle, or `NULL`; do not release the result.
void *rt_instbatch3d_borrow_mesh(void *batch);

/// @brief Internal bridge: borrow the batch's retained material (NULL for invalid handles).
/// @param batch InstanceBatch3D receiver.
/// @return Borrowed live Material3D handle, or `NULL`; do not release the result.
void *rt_instbatch3d_borrow_material(void *batch);

/// @brief Internal bridge: borrow the batch's sanitized float transform array (N * 16 values).
/// @param batch InstanceBatch3D receiver.
/// @param out_count Optional output initialized to zero and set to the matrix count on success.
/// @return Borrowed contiguous float mirror, or `NULL` for invalid or empty state.
///   The pointer may be invalidated by subsequent batch mutation.
const float *rt_instbatch3d_borrow_transforms(void *batch, int32_t *out_count);

#ifdef __cplusplus
}
#endif
