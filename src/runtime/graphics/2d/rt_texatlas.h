//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_texatlas.h
// Purpose: 2D texture atlas with named rectangular regions over a Pixels buffer.
//          Enables efficient sprite sheet workflows by mapping string names to
//          sub-rectangles, so game code can reference frames by name instead of
//          raw pixel coordinates.
//
// Key invariants:
//   - The atlas retains a reference to its backing Pixels object.
//   - Region names longer than 31 bytes are rejected.
//   - Maximum 512 named regions per atlas.
//   - Region coordinates are bounds-checked against the Pixels dimensions at add
//     time; a region that falls outside the backing Pixels is rejected (traps).
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects.
//   - The backing Pixels reference is retained and released in the finalizer;
//     rt_texatlas_get_pixels() returns only a borrowed pointer.
//
// Links: src/runtime/graphics/2d/rt_texatlas.c (implementation),
//        src/runtime/graphics/2d/rt_spritebatch.h (batch drawing with atlas)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @file
/// @brief Declares fixed-capacity named TextureAtlas and batch-draw interfaces.

/// Runtime class tag used to validate opaque TextureAtlas handles.
#define RT_TEXATLAS_CLASS_ID 0x5650475445584154LL /* "VPGTEXAT" */

//=========================================================================
// TextureAtlas Creation
//=========================================================================

/// @brief Create a new texture atlas backed by a Pixels buffer.
/// @param pixels Backing Pixels object (retained by the atlas).
/// @return A caller-owned empty TextureAtlas, or `NULL` after an invalid-input
///         trap or allocation failure.
void *rt_texatlas_new(void *pixels);

/// @brief Create a texture atlas by slicing a Pixels buffer into a grid.
/// @details Complete cells are named `"0"`, `"1"`, and so on in row-major
///          order. Partial right/bottom edges are ignored and generation stops
///          at 512 regions.
/// @param pixels Backing Pixels object.
/// @param frame_w Positive width of each grid cell in pixels.
/// @param frame_h Positive height of each grid cell in pixels.
/// @return A caller-owned TextureAtlas, or `NULL` after an argument trap or
///         allocation failure.
void *rt_texatlas_load_grid(void *pixels, int64_t frame_w, int64_t frame_h);

//=========================================================================
// TextureAtlas Region Management
//=========================================================================

/// @brief Add a named rectangular region to the atlas.
/// @details Reusing a case-sensitive name replaces the existing rectangle.
///          Empty/overlong names, nonpositive sizes, out-of-bounds rectangles,
///          and adding beyond 512 regions trap.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name of at most 31 bytes.
/// @param x Nonnegative left edge in the backing Pixels.
/// @param y Nonnegative top edge in the backing Pixels.
/// @param w Positive region width.
/// @param h Positive region height.
void rt_texatlas_add(void *atlas, void *name, int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Check whether a named region exists.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @return `1` if found, otherwise `0`.
int8_t rt_texatlas_has(void *atlas, void *name);

/// @brief Get the X coordinate of a named region.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @return Region X, or `0` for invalid input or a missing name.
int64_t rt_texatlas_get_x(void *atlas, void *name);

/// @brief Get the Y coordinate of a named region.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @return Region Y, or `0` for invalid input or a missing name.
int64_t rt_texatlas_get_y(void *atlas, void *name);

/// @brief Get the width of a named region.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @return Region width, or `0` for invalid input or a missing name.
int64_t rt_texatlas_get_w(void *atlas, void *name);

/// @brief Get the height of a named region.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @return Region height, or `0` for invalid input or a missing name.
int64_t rt_texatlas_get_h(void *atlas, void *name);

/// @brief Get the backing Pixels object.
/// @param atlas Candidate TextureAtlas.
/// @return Borrowed backing Pixels pointer, or `NULL` for an invalid atlas.
void *rt_texatlas_get_pixels(void *atlas);

/// @brief Get the number of named regions.
/// @param atlas Candidate TextureAtlas.
/// @return Region count, or `0` for an invalid atlas.
int64_t rt_texatlas_region_count(void *atlas);

//=========================================================================
// SpriteBatch Atlas Drawing Extensions
//=========================================================================

/// @brief Draw a named atlas region through the sprite batch.
/// @details Queues native-size drawing at depth zero. Null inputs and missing
///          names are silent no-ops.
/// @param batch Active SpriteBatch.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
void rt_spritebatch_draw_atlas(void *batch, void *atlas, void *name, int64_t x, int64_t y);

/// @brief Draw a named atlas region with uniform scale.
/// @param batch Active SpriteBatch.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
/// @param scale Horizontal and vertical percentage scale.
void rt_spritebatch_draw_atlas_scaled(
    void *batch, void *atlas, void *name, int64_t x, int64_t y, int64_t scale);

/// @brief Draw a named atlas region with full transform.
/// @param batch Active SpriteBatch.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime region name.
/// @param x Destination top-left X before rotation recentering.
/// @param y Destination top-left Y before rotation recentering.
/// @param scale Horizontal and vertical percentage scale.
/// @param rotation Clockwise rotation in degrees.
/// @param depth Explicit SpriteBatch sort key.
void rt_spritebatch_draw_atlas_ex(void *batch,
                                  void *atlas,
                                  void *name,
                                  int64_t x,
                                  int64_t y,
                                  int64_t scale,
                                  int64_t rotation,
                                  int64_t depth);

#ifdef __cplusplus
}
#endif
