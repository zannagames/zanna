//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/anim/rt_morphtarget3d_internal.h
// Purpose: Narrow internal bulk-data boundary for MorphTarget3D persistence.
// Key invariants:
//   - Views expose borrowed, immutable shape storage only after handle validation.
//   - Bulk append requires an exact vertex-count match and finite float payloads.
// Ownership/Lifetime:
//   - Returned pointers remain owned by the MorphTarget3D and are invalidated by mutation.
//   - Append copies every supplied byte; the caller retains ownership of its buffers.
// Links: rt_morphtarget3d.c, rt_scene3d_vscn_save.c, rt_scene3d_vscn_load.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Internal bulk persistence boundary for MorphTarget3D shapes.
/// @details These routines deliberately expose only validated immutable views
///          and whole-shape append. Views borrow container-owned storage and
///          should not survive mutation; append validates all lanes and copies
///          the complete payload before publishing a new shape.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Borrowed data for one validated morph shape.
typedef struct rt_morphtarget3d_shape_view_internal {
    /// Borrowed NUL-terminated name of at most 63 bytes.
    const char *name;
    /// Borrowed required array of `vertex_count * 3` position deltas.
    const float *position_deltas;
    /// Borrowed optional array of `vertex_count * 3` normal deltas.
    const float *normal_deltas;
    /// Borrowed optional array of `vertex_count * 3` tangent XYZ deltas.
    const float *tangent_deltas;
    /// Sanitized signed blend contribution in `[-1,1]`.
    double weight;
    /// Positive number of vertices represented by each present channel.
    int32_t vertex_count;
} rt_morphtarget3d_shape_view_internal;

/// @brief Borrow one shape's complete persistence payload.
/// @details Clears @p out_view before validation. The name and channel pointers
///          remain owned by the container and may be invalidated by later
///          mutation or finalization.
/// @param[in] morph MorphTarget3D to inspect.
/// @param[in] shape_index Zero-based shape index.
/// @param[out] out_view Writable view populated on success.
/// @return One on success; zero for a null/wrong-class handle or invalid index.
int8_t rt_morphtarget3d_get_shape_view_internal(void *morph,
                                                int64_t shape_index,
                                                rt_morphtarget3d_shape_view_internal *out_view);

/// @brief Copy one complete shape into an existing morph container.
/// @details The destination vertex count must equal `view->vertex_count`; position deltas are
/// required, normal/tangent deltas are optional, the name is at most 63 bytes,
/// and every weight/delta value must be finite. The append copies all supplied
/// storage, sanitizes weight to `[-1,1]`, initializes both history arrays to
/// that weight, and invalidates packed payload caches.
/// @param[in,out] morph Destination MorphTarget3D.
/// @param[in] view Borrowed complete shape payload.
/// @return One after publication; zero without publishing a usable shape on validation/OOM.
int8_t rt_morphtarget3d_append_shape_internal(void *morph,
                                              const rt_morphtarget3d_shape_view_internal *view);

#ifdef __cplusplus
}
#endif
