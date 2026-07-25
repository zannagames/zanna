//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/backend/vgfx3d_skinning.h
// Purpose: CPU-side vertex skinning — transforms vertices by a bone palette.
//   Each vertex has up to 4 bone indices + weights (from vgfx3d_vertex_t).
//
// Key invariants:
//   - Bone palette is bone_count * 16 floats (row-major 4x4 per bone).
//   - Weights are NOT required to sum to 1.0 (implicit normalization).
//   - Skinned normals are re-normalized after blending.
//   - src and dst may alias (in-place skinning).
//
// Ownership/Lifetime:
//   - Source, destination, and palette storage remain caller-owned.
//   - Optional scratch storage is caller-owned and reused across draws.
//
// Links: rt_canvas3d_internal.h (vgfx3d_vertex_t), plans/3d/14-skeletal-animation.md
//
//===----------------------------------------------------------------------===//

/**
 * @file vgfx3d_skinning.h
 * @brief Declares CPU linear-blend skinning for Graphics3D vertex streams.
 *
 * Positions are transformed by weighted affine bone matrices; normals and tangents use the
 * corresponding inverse-transpose matrices and are normalized after blending. The destination
 * may alias the source, weights need not sum to one, and optional caller-owned scratch storage
 * amortizes palette validation and normal-matrix derivation across vertices.
 */
#pragma once

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_canvas3d_internal.h"
#include "vgfx3d_skinning_scratch.h"
#include <stdint.h>

/// @brief Apply skeletal skinning on the CPU.
/// @details Transforms @p vertex_count vertices using both the four influences stored in each
///          vertex and optional per-vertex influences five through eight in @p extra.
/// @param src Borrowed source array of @p vertex_count vertices.
/// @param dst Caller-owned destination array; may equal @p src for in-place skinning.
/// @param vertex_count Number of vertices and, when present, side-stream entries to process.
/// @param palette Borrowed row-major palette of @p bone_count consecutive 4×4 matrices.
/// @param bone_count Palette size, clamped to the 256 indices representable by vertex records.
/// @param extra Optional borrowed array containing four additional influences per vertex.
/// @param scratch Optional caller-owned grow-only cache for normal matrices and validity flags.
void vgfx3d_skin_vertices_extra(const vgfx3d_vertex_t *src,
                                vgfx3d_vertex_t *dst,
                                uint32_t vertex_count,
                                const float *palette,
                                int32_t bone_count,
                                const vgfx3d_extra_influences_t *extra,
                                vgfx3d_skinning_scratch_t *scratch);

/// @brief CPU-skin vertices using the four bone influences stored in each vertex record.
/// @details This convenience wrapper delegates to vgfx3d_skin_vertices_extra() without an extra
///          influence side stream. Attributes other than transformed position, normal, and tangent
///          are sanitized and copied through.
/// @param src Borrowed source array of @p vertex_count vertices.
/// @param dst Caller-owned destination array; may equal @p src for in-place skinning.
/// @param vertex_count Number of vertices to process.
/// @param bone_palette Borrowed row-major palette of @p bone_count consecutive 4×4 matrices.
/// @param bone_count Palette size, clamped to the 256 indices representable by vertex records.
/// @param scratch Optional caller-owned grow-only cache for normal matrices and validity flags.
void vgfx3d_skin_vertices(const vgfx3d_vertex_t *src,
                          vgfx3d_vertex_t *dst,
                          uint32_t vertex_count,
                          const float *bone_palette,
                          int32_t bone_count,
                          vgfx3d_skinning_scratch_t *scratch);

#endif /* ZANNA_ENABLE_GRAPHICS */
