//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/world/rt_terrain3d_internal.h
// Purpose: Narrow, layout-independent Terrain3D queries shared by sibling 3D
//   runtime modules.
// Key invariants:
//   - Terrain3D object payload fields remain private to rt_terrain3d.c.
//   - Successful descriptors contain validated finite dimensions and extents.
// Ownership/Lifetime:
//   - Input Terrain3D handles are borrowed for the duration of each call.
//   - All returned descriptor data is copied into caller-owned storage.
// Links: rt_terrain3d.c, rt_vegetation3d.c,
// docs/adr/0173-graphics3d-transactional-hardening-and-retained-work.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_terrain3d_internal.h
 * @brief Declares layout-independent Terrain3D metadata shared by sibling runtime modules.
 *
 * The copied grid descriptor lets consumers reason about terrain dimensions and world extents
 * without duplicating or depending on the private `rt_terrain3d` object payload.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Validated, layout-independent description of a Terrain3D height grid.
/// @details The width and depth count height samples, not cells. Consequently,
///   each horizontal extent spans one fewer interval than its corresponding
///   sample count. Values are copied from the terrain only after its allocated
///   height buffer and scale have passed integrity checks.
/// @ownership This plain-data value owns no runtime references or allocations.
typedef struct rt_terrain3d_grid_info {
    int32_t width;       ///< Number of height samples along the local X axis.
    int32_t depth;       ///< Number of height samples along the local Z axis.
    double spacing_x;    ///< Positive world-space distance between adjacent X samples.
    double height_scale; ///< Finite multiplier applied to normalized height samples.
    double spacing_z;    ///< Positive world-space distance between adjacent Z samples.
    double extent_x;     ///< World-space X span: `(width - 1) * spacing_x`.
    double extent_z;     ///< World-space Z span: `(depth - 1) * spacing_z`.
} rt_terrain3d_grid_info;

/// @brief Copy validated Terrain3D grid metadata without exposing its private payload layout.
/// @param terrain Borrowed opaque Terrain3D handle to inspect.
/// @param out_info Caller-owned destination that receives the complete descriptor.
/// @return `1` when @p terrain is a live Terrain3D with a valid allocated grid,
///   positive finite X/Z spacing, finite height scale, and finite extents;
///   otherwise `0`.
/// @details The destination is zero-initialized before validation, including on
///   null or wrong-class handles. The function never retains @p terrain and
///   never returns pointers into its storage, so callers cannot depend on the
///   Terrain3D object layout or outlive borrowed internal buffers.
int8_t rt_terrain3d_get_grid_info_internal(void *terrain, rt_terrain3d_grid_info *out_info);

#ifdef __cplusplus
}
#endif
