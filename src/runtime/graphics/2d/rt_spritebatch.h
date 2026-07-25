//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_spritebatch.h
// Purpose: Public C ABI for recording retained Sprite, Pixels, and Pixels-region
// draw commands, optionally sorting them by depth, and rendering them to a
// Canvas in one native traversal with shared tint and alpha.
//
// Key invariants:
//   - Begin must be called before any draw calls; End flushes the batch.
//   - Draw calls preserve submission order unless depth sorting is enabled.
//   - When depth sorting is enabled, equal-depth items still preserve their
//     original submission order.
//   - Batch size is bounded by RT_SPRITEBATCH_MAX_SPRITES per begin/end pair.
//   - Nested Begin/End pairs are not supported.
//   - Begin discards any queued commands. End also clears commands, including
//     when passed a null Canvas, while retaining allocation capacity for reuse.
//   - For Sprite draw commands, the batch stores a retained reference to the
//     Sprite and samples its mutable state (current frame, flip flags, origin,
//     visibility) at End() time, NOT at the draw-submit call. Only the sort key
//     (depth) is snapshotted at submission. Therefore mutating a Sprite between
//     its draw-submit and End() affects that queued draw; submit a distinct
//     Sprite (or a Pixels/region command) per visual state if that matters.
//   - Batch tint, alpha, and sorting settings are sampled at End and persist
//     until explicitly changed or reset.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects.
//   - Each accepted command retains its source until the batch is cleared,
//     ended, begun again, or finalized.
//
// Links: src/runtime/graphics/2d/rt_spritebatch.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @file
/// @brief Declares retained-command SpriteBatch creation, drawing, and settings.

/// Runtime class tag used to validate opaque SpriteBatch handles.
#define RT_SPRITEBATCH_CLASS_ID INT64_C(-0x520103)

//=========================================================================
// SpriteBatch Creation
//=========================================================================

/// @brief Create a new sprite batch.
/// @details The new batch is inactive, preserves submission order, applies no
///          tint, and uses fully opaque global alpha.
/// @param capacity Initial command capacity. Nonpositive values select 256;
///                 values above 1,048,576 are capped.
/// @return A caller-owned SpriteBatch reference, or `NULL` on allocation failure.
void *rt_spritebatch_new(int64_t capacity);

//=========================================================================
// SpriteBatch Operations
//=========================================================================

/// @brief Begin a new batch.
/// @param batch SpriteBatch object.
/// @note Clears and releases pending draws from any previous batch while
///       preserving allocation capacity and render settings.
void rt_spritebatch_begin(void *batch);

/// @brief End the batch and draw all sprites to the canvas.
/// @param batch SpriteBatch object.
/// @param canvas Canvas to draw on.
/// @note Sprites are drawn in submission order unless depth sorting is enabled.
///       Pending draw commands are cleared whether or not a canvas is provided.
///       Calling End on an inactive batch is a no-op.
void rt_spritebatch_end(void *batch, void *canvas);

/// @brief Draw a sprite at the given position.
/// @details Queues at 100% scale and zero rotation. The command snapshots the
///          Sprite depth but reads its other mutable rendering state at End.
/// @param batch SpriteBatch object.
/// @param sprite Sprite object to draw.
/// @param x X position.
/// @param y Y position.
void rt_spritebatch_draw(void *batch, void *sprite, int64_t x, int64_t y);

/// @brief Draw a sprite with scale.
/// @details Queues a uniform percentage scale and zero rotation.
/// @param batch SpriteBatch object.
/// @param sprite Sprite object to draw.
/// @param x X position.
/// @param y Y position.
/// @param scale Scale factor (100 = 100%).
void rt_spritebatch_draw_scaled(void *batch, void *sprite, int64_t x, int64_t y, int64_t scale);

/// @brief Draw a sprite with full transform.
/// @details The Sprite is retained. Position, transform, and depth are
///          snapshotted while other Sprite drawing state is read at End.
/// @param batch SpriteBatch object.
/// @param sprite Sprite object to draw.
/// @param x X position.
/// @param y Y position.
/// @param scale_x Horizontal scale (100 = 100%).
/// @param scale_y Vertical scale (100 = 100%).
/// @param rotation Rotation in degrees.
void rt_spritebatch_draw_ex(void *batch,
                            void *sprite,
                            int64_t x,
                            int64_t y,
                            int64_t scale_x,
                            int64_t scale_y,
                            int64_t rotation);

/// @brief Draw a Pixels buffer directly.
/// @details Queues a whole-image alpha blit at depth zero and retains @p pixels.
/// @param batch SpriteBatch object.
/// @param pixels Pixels object to draw.
/// @param x X position.
/// @param y Y position.
void rt_spritebatch_draw_pixels(void *batch, void *pixels, int64_t x, int64_t y);

/// @brief Draw a sub-region of a Pixels buffer.
/// @details Queues native-size, unrotated drawing at depth zero and retains the
///          Pixels source.
/// @param batch SpriteBatch object.
/// @param pixels Pixels object.
/// @param dx Destination X.
/// @param dy Destination Y.
/// @param sx Source X.
/// @param sy Source Y.
/// @param sw Source width.
/// @param sh Source height.
void rt_spritebatch_draw_region(void *batch,
                                void *pixels,
                                int64_t dx,
                                int64_t dy,
                                int64_t sx,
                                int64_t sy,
                                int64_t sw,
                                int64_t sh);

/// @brief Internal/shared helper for region draws with transform metadata.
/// @details Used by TextureAtlas-backed draws so region items can participate
///          in depth sorting and transformed rendering without exposing a new
///          public language API surface. Scaling is percentage-based and the
///          rotated result is recentered around the unrotated region.
/// @param batch Active SpriteBatch.
/// @param pixels Valid Pixels source retained by the command.
/// @param dx Destination top-left X before rotation recentering.
/// @param dy Destination top-left Y before rotation recentering.
/// @param sx Source rectangle X.
/// @param sy Source rectangle Y.
/// @param sw Source rectangle width.
/// @param sh Source rectangle height.
/// @param scale_x Horizontal percentage scale.
/// @param scale_y Vertical percentage scale.
/// @param rotation Clockwise rotation in degrees.
/// @param depth Explicit depth-sort key.
void rt_spritebatch_draw_region_ex(void *batch,
                                   void *pixels,
                                   int64_t dx,
                                   int64_t dy,
                                   int64_t sx,
                                   int64_t sy,
                                   int64_t sw,
                                   int64_t sh,
                                   int64_t scale_x,
                                   int64_t scale_y,
                                   int64_t rotation,
                                   int64_t depth);

//=========================================================================
// SpriteBatch Properties
//=========================================================================

/// @brief Get the number of sprites currently in the batch.
/// @param batch Candidate SpriteBatch.
/// @return Number of queued commands, or `0` for an invalid handle.
int64_t rt_spritebatch_count(void *batch);

/// @brief Get the current capacity of the batch.
/// @param batch Candidate SpriteBatch.
/// @return Number of allocated command slots, or `0` for an invalid handle.
int64_t rt_spritebatch_capacity(void *batch);

/// @brief Check if the batch is currently recording.
/// @param batch Candidate SpriteBatch.
/// @return `1` between Begin and active End, otherwise `0`.
int8_t rt_spritebatch_is_active(void *batch);

//=========================================================================
// SpriteBatch Settings
//=========================================================================

/// @brief Set whether to sort sprites by depth before rendering.
/// @details Equal-depth commands continue to render in submission order.
/// @param batch SpriteBatch object.
/// @param enabled 1 to enable depth sorting, 0 to disable.
void rt_spritebatch_set_sort_by_depth(void *batch, int8_t enabled);

/// @brief Set a global tint color for all sprites in the batch.
/// @details The setting is sampled at End and persists across batches.
/// @param batch SpriteBatch object.
/// @param color Tint color (0xAARRGGBB or 0x00RRGGBB), or -1 for no tint.
void rt_spritebatch_set_tint(void *batch, int64_t color);

/// @brief Set a global alpha for all sprites in the batch.
/// @details The value is clamped and sampled at End.
/// @param batch SpriteBatch object.
/// @param alpha Alpha value (0-255).
void rt_spritebatch_set_alpha(void *batch, int64_t alpha);

/// @brief Clear all settings to defaults.
/// @details Restores submission ordering, no tint, and alpha 255 without
///          changing queued commands, capacity, or active state.
/// @param batch Candidate SpriteBatch.
void rt_spritebatch_reset_settings(void *batch);

//=========================================================================
// SpriteBatch Atlas Drawing Extensions (from rt_texatlas.h)
//=========================================================================

/// @brief Draw a named atlas region through the sprite batch.
/// @details Resolves the runtime-string name and queues a native-size region at
///          depth zero. Null inputs or a missing region are silent no-ops.
/// @param batch Active SpriteBatch.
/// @param atlas Valid TextureAtlas.
/// @param name Borrowed runtime string naming a region.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
void rt_spritebatch_draw_atlas(void *batch, void *atlas, void *name, int64_t x, int64_t y);

/// @brief Draw a named atlas region with uniform scale.
/// @details Delegates to the transformed region command at zero rotation/depth.
/// @param batch Active SpriteBatch.
/// @param atlas Valid TextureAtlas.
/// @param name Borrowed runtime string naming a region.
/// @param x Destination top-left X coordinate.
/// @param y Destination top-left Y coordinate.
/// @param scale Horizontal and vertical percentage scale.
void rt_spritebatch_draw_atlas_scaled(
    void *batch, void *atlas, void *name, int64_t x, int64_t y, int64_t scale);

/// @brief Draw a named atlas region with full transform.
/// @param batch Active SpriteBatch.
/// @param atlas Valid TextureAtlas.
/// @param name Borrowed runtime string naming a region.
/// @param x Destination top-left X before rotation recentering.
/// @param y Destination top-left Y before rotation recentering.
/// @param scale Horizontal and vertical percentage scale.
/// @param rotation Clockwise rotation in degrees.
/// @param depth Explicit depth-sort key.
void rt_spritebatch_draw_atlas_ex(void *batch,
                                  void *atlas,
                                  void *name,
                                  int64_t x,
                                  int64_t y,
                                  int64_t scale,
                                  int64_t rotation,
                                  int64_t depth);

//=========================================================================
// TextureAtlas
//
// Convenience re-declarations of the named-region TextureAtlas API. These are
// the same entry points declared in rt_texatlas.h.
//=========================================================================

/// @brief Create an empty named-region atlas over retained Pixels.
/// @param pixels Valid backing Pixels object retained by the atlas.
/// @return A caller-owned TextureAtlas reference, or `NULL` on invalid input or
///         allocation failure.
void *rt_texatlas_new(void *pixels);

/// @brief Slice retained Pixels into row-major, numerically named grid regions.
/// @param pixels Valid backing Pixels object.
/// @param frame_w Positive grid-cell width.
/// @param frame_h Positive grid-cell height.
/// @return A caller-owned TextureAtlas with names `"0"`, `"1"`, and so on,
///         or `NULL` on invalid input, excessive cells, or allocation failure.
void *rt_texatlas_load_grid(void *pixels, int64_t frame_w, int64_t frame_h);

/// @brief Add or replace a case-sensitive named rectangle.
/// @param atlas Valid TextureAtlas.
/// @param name Borrowed runtime string of at most 31 bytes.
/// @param x Nonnegative source X coordinate.
/// @param y Nonnegative source Y coordinate.
/// @param w Positive source width.
/// @param h Positive source height.
void rt_texatlas_add(void *atlas, void *name, int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Test whether a named region exists.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime string naming the region.
/// @return `1` when found, otherwise `0`.
int8_t rt_texatlas_has(void *atlas, void *name);

/// @brief Get a named region's source X coordinate.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime string naming the region.
/// @return Region X, or `0` when input is invalid or the name is absent.
int64_t rt_texatlas_get_x(void *atlas, void *name);

/// @brief Get a named region's source Y coordinate.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime string naming the region.
/// @return Region Y, or `0` when input is invalid or the name is absent.
int64_t rt_texatlas_get_y(void *atlas, void *name);

/// @brief Get a named region's source width.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime string naming the region.
/// @return Region width, or `0` when input is invalid or the name is absent.
int64_t rt_texatlas_get_w(void *atlas, void *name);

/// @brief Get a named region's source height.
/// @param atlas Candidate TextureAtlas.
/// @param name Borrowed runtime string naming the region.
/// @return Region height, or `0` when input is invalid or the name is absent.
int64_t rt_texatlas_get_h(void *atlas, void *name);

/// @brief Borrow the atlas's backing Pixels object.
/// @param atlas Candidate TextureAtlas.
/// @return Borrowed Pixels pointer, or `NULL` for an invalid atlas.
void *rt_texatlas_get_pixels(void *atlas);

/// @brief Get the number of named regions.
/// @param atlas Candidate TextureAtlas.
/// @return Region count, or `0` for an invalid atlas.
int64_t rt_texatlas_region_count(void *atlas);

#ifdef __cplusplus
}
#endif
