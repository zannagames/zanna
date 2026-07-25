//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_sprite3d.h
// Purpose: 3D sprite — camera-facing textured billboard with sprite sheet
//   frame support. Used for 2D characters in 3D worlds, RPG items, trees.
//
// Key invariants:
//   - Always faces camera (billboard rendering).
//   - Frame rect selects sub-region of texture atlas.
//   - Anchor point controls pivot (0.5,0.5 = center).
//
// Links: rt_canvas3d.h, rt_particles3d.c (billboard math)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the camera-facing Sprite3D billboard C ABI.
/// @details Sprites retain an optional Pixels texture, select spritesheet regions, expose
///   sanitized world-space placement and appearance controls, and generate camera-relative quad
///   geometry when submitted to a Canvas3D.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a billboarded 3D sprite from @p texture.
/// @param texture Optional non-empty Pixels object retained by the sprite.
/// @return A new GC-managed Sprite3D, or `NULL` after trapping on invalid texture or allocation
///   failure.
void *rt_sprite3d_new(void *texture);

/// @brief Set the sprite's world position.
/// @param spr Sprite3D instance.
/// @param x World-space x coordinate.
/// @param y World-space y coordinate.
/// @param z World-space z coordinate.
void rt_sprite3d_set_position(void *spr, double x, double y, double z);

/// @brief Set the sprite's world-space width/height.
/// @param spr Sprite3D instance.
/// @param w Positive billboard width.
/// @param h Positive billboard height.
void rt_sprite3d_set_scale(void *spr, double w, double h);

/// @brief Set the pivot/anchor in [0,1] sprite-local coords (0.5,0.5 = center).
/// @param spr Sprite3D instance.
/// @param ax Horizontal anchor clamped to the unit interval.
/// @param ay Vertical anchor clamped to the unit interval.
void rt_sprite3d_set_anchor(void *spr, double ax, double ay);

/// @brief Select a sub-rectangle (fx,fy,fw,fh) of the texture as the frame
///        (for sprite-sheet animation).
/// @param spr Sprite3D instance.
/// @param fx Horizontal frame origin in texture pixels.
/// @param fy Vertical frame origin in texture pixels.
/// @param fw Frame width; non-positive input selects the available texture width.
/// @param fh Frame height; non-positive input selects the available texture height.
void rt_sprite3d_set_frame(void *spr, int64_t fx, int64_t fy, int64_t fw, int64_t fh);

/// @brief Shift sprite world-space state by the inverse of a floating-origin delta.
/// @param spr Sprite3D instance.
/// @param dx World-space x displacement subtracted from the stored position.
/// @param dy World-space y displacement subtracted from the stored position.
/// @param dz World-space z displacement subtracted from the stored position.
void rt_sprite3d_rebase_origin(void *spr, double dx, double dy, double dz);

/// @brief Toggle additive blending (glows/tracers) vs default alpha blending.
/// @param spr Sprite3D instance.
/// @param additive Non-zero for additive blending, or zero for the non-additive path.
void rt_sprite3d_set_additive(void *spr, int8_t additive);

/// @brief Current additive-blend flag.
/// @param spr Candidate Sprite3D instance.
/// @return 1 when additive blending is enabled, or 0 for non-additive or invalid input.
int8_t rt_sprite3d_get_additive(void *spr);

/// @brief Set a packed 0xRRGGBB tint multiplied into the sprite texture.
/// @param spr Sprite3D instance.
/// @param rgb Packed red, green, and blue bytes; higher bits are ignored.
void rt_sprite3d_set_color(void *spr, int64_t rgb);

/// @brief Draw @p sprite billboarded toward @p camera onto the 3D canvas.
/// @param canvas Canvas3D receiving the deferred billboard draw.
/// @param sprite Sprite3D instance to render.
/// @param camera Camera3D whose view basis orients the billboard.
void rt_canvas3d_draw_sprite3d(void *canvas, void *sprite, void *camera);

#ifdef __cplusplus
}
#endif
