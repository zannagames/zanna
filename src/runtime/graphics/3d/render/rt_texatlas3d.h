//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_texatlas3d.h
// Purpose: Texture atlas — packs multiple textures into one large texture
//   to reduce texture switches during rendering.
//
// Key invariants:
//   - Shelf-based bin packing (row-by-row placement).
//   - 1-pixel border padding to prevent texture bleeding.
//   - UV rects returned as normalized [0,1] coordinates.
//
// Links: rt_canvas3d.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the shelf-packed TextureAtlas3D C ABI.
/// @details Atlases accept up to 256 copied Pixels regions, replicate one-pixel edge borders to
///   prevent sampling bleed, expose stable normalized UV rectangles, and lazily provide a borrowed
///   Pixels mirror of the packed texture.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a 3D texture atlas of the given pixel dimensions.
/// @param width Atlas width from 16 through 8192 pixels.
/// @param height Atlas height from 16 through 8192 pixels.
/// @return A new GC-managed TextureAtlas3D, or `NULL` after trapping on invalid dimensions or
///   allocation failure.
void *rt_texatlas3d_new(int64_t width, int64_t height);

/// @brief Pack a Pixels image into the atlas; returns its region id (or -1).
/// @param atlas TextureAtlas3D receiving the copied image.
/// @param pixels Non-empty Pixels source; the atlas does not retain it.
/// @return A stable non-negative region identifier, or -1 when input is invalid, capacity is
///   exhausted, or the image does not fit.
int64_t rt_texatlas3d_add(void *atlas, void *pixels);

/// @brief Get the GPU/runtime texture handle backing the whole atlas.
/// @param atlas Candidate TextureAtlas3D instance.
/// @return A borrowed atlas-owned Pixels mirror, rebuilt after packing changes, or `NULL` on
///   failure.
void *rt_texatlas3d_get_texture(void *atlas);

/// @brief Get the normalized UV rect (u0,v0)-(u1,v1) for a packed region id.
/// @param atlas Candidate TextureAtlas3D instance.
/// @param id Region identifier returned by `rt_texatlas3d_add`.
/// @param[out] u0 Required destination for the left coordinate.
/// @param[out] v0 Required destination for the top coordinate.
/// @param[out] u1 Required destination for the right coordinate.
/// @param[out] v1 Required destination for the bottom coordinate.
void rt_texatlas3d_get_uv_rect(
    void *atlas, int64_t id, double *u0, double *v0, double *u1, double *v1);

/// @brief `TextureAtlas3D.GetUvMin(id)` — top-left UV of a packed region (ADR 0227).
/// @param atlas Candidate TextureAtlas3D instance.
/// @param id Region identifier returned by `rt_texatlas3d_add`.
/// @return New Vec2 of (u0, v0); the full-atlas corner for invalid input.
void *rt_texatlas3d_get_uv_min(void *atlas, int64_t id);

/// @brief `TextureAtlas3D.GetUvMax(id)` — bottom-right UV of a packed region (ADR 0227).
/// @param atlas Candidate TextureAtlas3D instance.
/// @param id Region identifier returned by `rt_texatlas3d_add`.
/// @return New Vec2 of (u1, v1); the full-atlas corner for invalid input.
void *rt_texatlas3d_get_uv_max(void *atlas, int64_t id);

#ifdef __cplusplus
}
#endif
