//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_graphics2d.h
// Purpose: Runtime bridge declarations for the Zanna.Graphics 2D support layer —
// render targets, textures, batched/sprite renderers, materials, shaders and
// post-process effects, viewports, tile maps, paths, shape/text renderers,
// transforms, sampler/blend state, animation, render graphs, collision masks,
// palettes, gradients, camera rigs, and asset importers.
//
// Key invariants:
//   - Every object is a heap-allocated opaque `void *` handle; APIs never expose
//     concrete struct layouts to the caller.
//   - Colors are packed integers: 0xRRGGBBAA for raw pixel/RGBA parameters and
//     0x00RRGGBB for "rgb"/"tint" parameters, matching rt_pixels conventions.
//   - Coordinates are 0-based from the top-left; out-of-range tile/region access
//     is clipped or returns a zero/identity/sentinel value rather than trapping
//     (e.g. rt_tilelayer2d_get returns -1 for an out-of-bounds cell).
//   - Percent-style parameters (lerp_pct, t_pct, opacity, alpha) are integers in
//     0..100 unless a wider range is documented on the specific function; byte
//     alpha parameters document 0..255.
//
// Ownership/Lifetime:
//   - `*_new` constructors return owned handles; the caller manages lifetime.
//   - `*_apply` helpers that return a `void *` produce a new Pixels/handle owned
//     by the caller; inputs are not consumed.
//
// Links: src/runtime/graphics/2d/rt_graphics2d.c (implementation),
//        src/runtime/graphics/2d/rt_pixels.h (Pixels buffer type),
//        src/runtime/game/rt_tiled_import.cpp (Tiled file adapters)
//
//===----------------------------------------------------------------------===//
#pragma once

/// @file
/// @brief Declares the opaque C ABI for Zanna's CPU-backed 2D graphics layer.
///
/// This header groups the runtime entry points used to allocate, configure,
/// query, and compose the 2D graphics object families.  Every object parameter
/// is an opaque runtime handle; callers must pass the class documented for that
/// parameter and must not inspect or free its implementation storage directly.
/// Constructors and image-producing operations return managed runtime objects,
/// while getters explicitly identified as borrowed do not transfer ownership.
///
/// Unless a function documents a trap, invalid object handles are rejected
/// softly: mutators do nothing, pointer results are `NULL`, and scalar queries
/// return the stated default or sentinel.  Pixel operations clip to their
/// destination, and queued renderer commands retain their sources until the
/// queue is cleared, ended, or finalized.

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Texture sampling filter modes (nearest-neighbor vs bilinear).
enum {
    /// Selects nearest-neighbor sampling for crisp, unfiltered texels.
    RT_GRAPHICS2D_FILTER_NEAREST = 0,
    /// Selects bilinear sampling when a texture is scaled.
    RT_GRAPHICS2D_FILTER_LINEAR = 1,
};

/// @brief Texture coordinate wrap modes at sampling edges.
enum {
    /// Clamps out-of-range coordinates to the nearest edge texel.
    RT_GRAPHICS2D_WRAP_CLAMP = 0,
    /// Repeats the texture periodically outside its native bounds.
    RT_GRAPHICS2D_WRAP_REPEAT = 1,
};

/// @brief Blend modes used when compositing source pixels onto a destination.
enum {
    RT_GRAPHICS2D_BLEND_ALPHA = 0,  ///< Standard source-over alpha blending.
    RT_GRAPHICS2D_BLEND_OPAQUE = 1, ///< Source replaces destination (no blend).
    RT_GRAPHICS2D_BLEND_ADD = 2,    ///< Additive blending (for glows/particles).
};

/// @brief Image post-process effect kinds for shader2d/postprocess2d.
enum {
    /// Copies the source without modifying its colors.
    RT_GRAPHICS2D_EFFECT_NONE = 0,
    /// Inverts the source RGB channels.
    RT_GRAPHICS2D_EFFECT_INVERT = 1,
    /// Converts the source to grayscale.
    RT_GRAPHICS2D_EFFECT_GRAYSCALE = 2,
    /// Multiplies the source by the configured tint color.
    RT_GRAPHICS2D_EFFECT_TINT = 3,
    /// Applies a box blur using the configured amount as its radius.
    RT_GRAPHICS2D_EFFECT_BLUR = 4,
};

//===----------------------------------------------------------------------===//
// RenderTarget2D — an offscreen RGBA surface that can be drawn into.
//===----------------------------------------------------------------------===//

/// @brief Creates a new offscreen render target of the given pixel size.
/// @param width Requested width in pixels; must form a valid allocation with
///        @p height.
/// @param height Requested height in pixels; must form a valid allocation with
///        @p width.
/// @return An owned RenderTarget2D handle, or `NULL` if allocation fails.
/// @details Invalid or overflowing dimensions raise a runtime trap.
void *rt_rendertarget2d_new(int64_t width, int64_t height);
/// @brief Creates a Surface2D with RenderTarget2D behavior and distinct type identity.
/// @param width Requested surface width in pixels.
/// @param height Requested surface height in pixels.
/// @return An owned Surface2D handle, or `NULL` if allocation fails.
/// @details Invalid or overflowing dimensions raise a runtime trap.
void *rt_surface2d_new(int64_t width, int64_t height);
/// @brief Gets the render target width in pixels.
/// @param target RenderTarget2D or Surface2D handle to inspect.
/// @return The backing image width, or `0` for an invalid handle.
int64_t rt_rendertarget2d_width(void *target);
/// @brief Gets the render target height in pixels.
/// @param target RenderTarget2D or Surface2D handle to inspect.
/// @return The backing image height, or `0` for an invalid handle.
int64_t rt_rendertarget2d_height(void *target);
/// @brief Borrows the target's backing Pixels buffer.
/// @param target RenderTarget2D or Surface2D handle to inspect.
/// @return The borrowed Pixels handle, or `NULL` for an invalid target.
/// @warning The returned handle remains owned by @p target and must not be
///          released by the caller.
void *rt_rendertarget2d_get_pixels(void *target);
/// @brief Clears the entire target to a packed RGBA color.
/// @param target Render target whose backing image is filled.
/// @param rgba Fill color encoded as `0xRRGGBBAA`.
void rt_rendertarget2d_clear(void *target, int64_t rgba);
/// @brief Resizes the target by replacing its backing Pixels buffer.
/// @param target Render target to resize.
/// @param width Requested replacement width in pixels.
/// @param height Requested replacement height in pixels.
/// @details Invalid dimensions trap.  An allocation failure leaves the original
///          image and dimensions intact.
void rt_rendertarget2d_resize(void *target, int64_t width, int64_t height);
/// @brief Alpha-composites a whole Pixels image into the target.
/// @param target Destination render target.
/// @param x Destination X coordinate of the source's top-left corner.
/// @param y Destination Y coordinate of the source's top-left corner.
/// @param pixels Borrowed source Pixels handle.
/// @details Drawing is clipped to the destination and does not consume the
///          source image.
void rt_rendertarget2d_draw_pixels(void *target, int64_t x, int64_t y, void *pixels);
/// @brief Alpha-composites a source sub-rectangle into the target.
/// @param target Destination render target.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param pixels Borrowed source Pixels handle.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Requested source rectangle width.
/// @param height Requested source rectangle height.
/// @details Source and destination rectangles are clipped to their respective
///          images; invalid handles are ignored.
void rt_rendertarget2d_draw_region(void *target,
                                   int64_t x,
                                   int64_t y,
                                   void *pixels,
                                   int64_t sx,
                                   int64_t sy,
                                   int64_t width,
                                   int64_t height);

//===----------------------------------------------------------------------===//
// Texture2D — an immutable-ish image with filter/wrap sampling state.
//===----------------------------------------------------------------------===//

/// @brief Creates a CPU-backed texture from an existing Pixels image.
/// @param pixels Valid Pixels handle retained by the new texture.
/// @return An owned Texture2D handle, or `NULL` for an invalid source or
///         allocation failure.
/// @details The texture starts with nearest filtering and clamp wrapping.
void *rt_texture2d_new(void *pixels);
/// @brief Creates a GpuTexture2D with Texture2D behavior and distinct type identity.
/// @param pixels Valid Pixels handle retained by the new texture.
/// @return An owned GpuTexture2D handle, or `NULL` on failure.
/// @details This implementation remains CPU-backed; the class identity reserves
///          the GPU-oriented API surface.
void *rt_gputexture2d_new(void *pixels);
/// @brief Loads an image file and wraps it in a Texture2D.
/// @param path Filesystem path accepted by the Pixels image loader.
/// @return An owned Texture2D handle, or `NULL` if loading or allocation fails.
void *rt_texture2d_from_file(rt_string path);
/// @brief Loads an image file and wraps it in a GpuTexture2D.
/// @param path Filesystem path accepted by the Pixels image loader.
/// @return An owned GpuTexture2D handle, or `NULL` if loading or allocation fails.
void *rt_gputexture2d_from_file(rt_string path);
/// @brief Gets the texture width in pixels.
/// @param texture Texture2D or GpuTexture2D handle to inspect.
/// @return The source image width, or `0` for an invalid handle.
int64_t rt_texture2d_width(void *texture);
/// @brief Gets the texture height in pixels.
/// @param texture Texture2D or GpuTexture2D handle to inspect.
/// @return The source image height, or `0` for an invalid handle.
int64_t rt_texture2d_height(void *texture);
/// @brief Borrows the texture's backing Pixels buffer.
/// @param texture Texture2D or GpuTexture2D handle to inspect.
/// @return The borrowed Pixels handle, or `NULL` for an invalid texture.
/// @warning The texture retains ownership; callers must not release the result.
void *rt_texture2d_get_pixels(void *texture);
/// @brief Allocates a copy of the texture's pixels.
/// @param texture Texture whose image is cloned.
/// @return A new owned Pixels handle, or `NULL` for an invalid texture or
///         allocation failure.
void *rt_texture2d_clone_pixels(void *texture);
/// @brief Sets the sampling filter used by subsequently queued texture draws.
/// @param texture Texture whose state is changed.
/// @param filter @ref RT_GRAPHICS2D_FILTER_LINEAR for bilinear sampling; every
///        other value selects nearest-neighbor sampling.
void rt_texture2d_set_filter(void *texture, int64_t filter);
/// @brief Gets the current texture sampling filter.
/// @param texture Texture to inspect.
/// @return A `RT_GRAPHICS2D_FILTER_*` value, defaulting to nearest for an
///         invalid handle.
int64_t rt_texture2d_get_filter(void *texture);
/// @brief Sets the coordinate wrap mode for texture-region sampling.
/// @param texture Texture whose state is changed.
/// @param wrap @ref RT_GRAPHICS2D_WRAP_REPEAT to repeat; every other value
///        selects edge clamping.
void rt_texture2d_set_wrap(void *texture, int64_t wrap);
/// @brief Gets the current texture-coordinate wrap mode.
/// @param texture Texture to inspect.
/// @return A `RT_GRAPHICS2D_WRAP_*` value, defaulting to clamp for an invalid
///         handle.
int64_t rt_texture2d_get_wrap(void *texture);

//===----------------------------------------------------------------------===//
// Renderer2D — an immediate/queued 2D draw list flushed to a target or canvas.
//===----------------------------------------------------------------------===//

/// @brief Creates an inactive retained-command 2D renderer.
/// @param capacity Requested initial command capacity, normalized to the
///        implementation's safe minimum and maximum.
/// @return An owned Renderer2D handle, or `NULL` if allocation fails.
/// @details The initial state is no tint, full alpha, and alpha blending.  Call
///          @ref rt_renderer2d_begin before queueing commands.
void *rt_renderer2d_new(int64_t capacity);
/// @brief Begins a new command frame.
/// @param renderer Renderer to activate.
/// @details Releases every previously retained command source, empties the
///          queue, and permits subsequent `draw_*` calls.
void rt_renderer2d_begin(void *renderer);
/// @brief Clears all queued draw calls without changing render state.
/// @param renderer Renderer whose commands are released.
/// @details Retained source handles are released and the command count becomes
///          zero; the renderer's active flag is otherwise unchanged.
void rt_renderer2d_clear(void *renderer);
/// @brief Gets the number of draw calls currently queued.
/// @param renderer Renderer to inspect.
/// @return The queued-command count, or `0` for an invalid handle.
int64_t rt_renderer2d_count(void *renderer);
/// @brief Sets the multiplicative tint captured by subsequent commands.
/// @param renderer Renderer whose current state is changed.
/// @param rgb Tint encoded as `0x00RRGGBB`; a negative value disables tinting.
void rt_renderer2d_set_tint(void *renderer, int64_t rgb);
/// @brief Sets the alpha scale captured by subsequent commands.
/// @param renderer Renderer whose current state is changed.
/// @param alpha Alpha in `0..255`; values outside the range are clamped.
void rt_renderer2d_set_alpha(void *renderer, int64_t alpha);
/// @brief Sets the blend mode captured by subsequent commands.
/// @param renderer Renderer whose current state is changed.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` value, clamped to the
///        supported numeric range.
void rt_renderer2d_set_blend_mode(void *renderer, int64_t blend_mode);
/// @brief Queues a draw of a raw Pixels image.
/// @param renderer Active renderer receiving the command.
/// @param pixels Valid Pixels source retained by the queued command.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @details Inactive renderers and invalid sources are ignored.
void rt_renderer2d_draw_pixels(void *renderer, void *pixels, int64_t x, int64_t y);
/// @brief Queues a full Texture2D draw using its current sampler state.
/// @param renderer Active renderer receiving the command.
/// @param texture Valid Texture2D source retained by the command.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
void rt_renderer2d_draw_texture(void *renderer, void *texture, int64_t x, int64_t y);
/// @brief Queues a full texture draw rotated about its center.
/// @param renderer Active renderer receiving the command.
/// @param texture Valid Texture2D source retained by the command.
/// @param x Destination X coordinate of the unrotated image.
/// @param y Destination Y coordinate of the unrotated image.
/// @param angle_deg Clockwise rotation in finite degrees, normalized modulo 360.
/// @details Non-finite angles and invalid or inactive handles are ignored.
void rt_renderer2d_draw_texture_rotated(
    void *renderer, void *texture, int64_t x, int64_t y, double angle_deg);
/// @brief Queues a full texture draw rotated about an explicit local pivot.
/// @param renderer Active renderer receiving the command.
/// @param texture Valid Texture2D source retained by the command.
/// @param x Destination X coordinate of the unrotated image.
/// @param y Destination Y coordinate of the unrotated image.
/// @param pivot_x Pivot X measured in texture-local pixels.
/// @param pivot_y Pivot Y measured in texture-local pixels.
/// @param angle_deg Clockwise rotation in finite degrees, normalized modulo 360.
void rt_renderer2d_draw_texture_rotated_at(void *renderer,
                                           void *texture,
                                           int64_t x,
                                           int64_t y,
                                           int64_t pivot_x,
                                           int64_t pivot_y,
                                           double angle_deg);
/// @brief Queues a full texture draw scaled to a destination rectangle.
/// @param renderer Active renderer receiving the command.
/// @param texture Valid Texture2D source retained by the command.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param width Positive destination width in pixels.
/// @param height Positive destination height in pixels.
/// @details Scaling uses the filter and wrap state captured from @p texture.
void rt_renderer2d_draw_texture_scaled(
    void *renderer, void *texture, int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Queues an unscaled sub-rectangle of a texture.
/// @param renderer Active renderer receiving the command.
/// @param texture Valid Texture2D source retained by the command.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Positive source and destination width.
/// @param height Positive source and destination height.
/// @details Out-of-range source coordinates use the texture's captured wrap mode.
void rt_renderer2d_draw_texture_region(void *renderer,
                                       void *texture,
                                       int64_t x,
                                       int64_t y,
                                       int64_t sx,
                                       int64_t sy,
                                       int64_t width,
                                       int64_t height);
/// @brief Queues an unscaled sub-rectangle of a raw Pixels image.
/// @param renderer Active renderer receiving the command.
/// @param pixels Valid Pixels source retained by the command.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Positive source and destination width.
/// @param height Positive source and destination height.
/// @details A null Pixels handle raises the Pixels validation trap; inactive
///          renderers or non-positive extents otherwise produce no command.
void rt_renderer2d_draw_region(void *renderer,
                               void *pixels,
                               int64_t x,
                               int64_t y,
                               int64_t sx,
                               int64_t sy,
                               int64_t width,
                               int64_t height);
/// @brief Flushes all queued draws into an offscreen render target.
/// @param renderer Renderer whose command stream is replayed.
/// @param target Destination RenderTarget2D or Surface2D.
/// @details Unlike @ref rt_renderer2d_end, this does not clear the queue or
///          deactivate the renderer.  Commands remain available for another
///          target; replaying alpha-blended commands into the same target
///          composites them again. If a texture is backed by the destination
///          Pixels, scaled and rotated draws snapshot the source before
///          writing so results do not depend on scan order.
void rt_renderer2d_flush_to_target(void *renderer, void *target);
/// @brief Ends a frame by flushing queued draws to a Canvas.
/// @param renderer Active renderer to flush and deactivate.
/// @param canvas Destination Canvas; `NULL` skips drawing but still ends and
///        clears an active frame.
/// @details Commands are executed in queue order, then their retained sources
///          are released, the queue is cleared, and a new @ref
///          rt_renderer2d_begin call is required.
void rt_renderer2d_end(void *renderer, void *canvas);

//===----------------------------------------------------------------------===//
// Material2D — reusable tint/alpha/blend state applied to an image.
//===----------------------------------------------------------------------===//

/// @brief Creates a material with identity tint, full alpha, and alpha blending.
/// @return An owned Material2D handle, or `NULL` if allocation fails.
void *rt_material2d_new(void);
/// @brief Sets the material's multiplicative tint.
/// @param material Material to update.
/// @param rgb Tint encoded as `0x00RRGGBB`; a negative value disables tinting.
void rt_material2d_set_tint(void *material, int64_t rgb);
/// @brief Gets the material's multiplicative tint.
/// @param material Material to inspect.
/// @return `0x00RRGGBB`, or `-1` when tinting is disabled or the handle is invalid.
int64_t rt_material2d_get_tint(void *material);
/// @brief Sets the material alpha scale.
/// @param material Material to update.
/// @param alpha Alpha in `0..255`; out-of-range values are clamped.
void rt_material2d_set_alpha(void *material, int64_t alpha);
/// @brief Gets the material alpha scale.
/// @param material Material to inspect.
/// @return Alpha in `0..255`, defaulting to `255` for an invalid handle.
int64_t rt_material2d_get_alpha(void *material);
/// @brief Sets the material blend mode.
/// @param material Material to update.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` value, clamped to `0..2`.
void rt_material2d_set_blend_mode(void *material, int64_t blend_mode);
/// @brief Gets the material blend mode.
/// @param material Material to inspect.
/// @return A `RT_GRAPHICS2D_BLEND_*` value, defaulting to alpha blending.
int64_t rt_material2d_get_blend_mode(void *material);
/// @brief Applies the material's tint and alpha to a copy of an image.
/// @param material Material supplying tint and alpha state.
/// @param pixels Borrowed source Pixels handle; it is never modified.
/// @return A new owned Pixels handle, or `NULL` for invalid input or allocation
///         failure.
/// @details Blend mode is compositing state and is not baked into the returned
///          image.
void *rt_material2d_apply(void *material, void *pixels);

//===----------------------------------------------------------------------===//
// Shader2D — a single named image effect with amount/color parameters.
//===----------------------------------------------------------------------===//

/// @brief Creates a CPU image-effect shader.
/// @param effect Requested `RT_GRAPHICS2D_EFFECT_*` value, clamped to `0..4`.
/// @return An owned Shader2D handle, or `NULL` if allocation fails.
/// @details The initial amount is `1` and the initial effect color is white.
void *rt_shader2d_new(int64_t effect);
/// @brief Sets the active image effect.
/// @param shader Shader to update.
/// @param effect Requested effect index, clamped to `0..4`.
void rt_shader2d_set_effect(void *shader, int64_t effect);
/// @brief Gets the active image effect.
/// @param shader Shader to inspect.
/// @return A `RT_GRAPHICS2D_EFFECT_*` value, defaulting to none.
int64_t rt_shader2d_get_effect(void *shader);
/// @brief Sets the effect-specific amount.
/// @param shader Shader to update.
/// @param amount Requested amount clamped to `0..10`; blur interprets it as
///        its radius.
void rt_shader2d_set_amount(void *shader, int64_t amount);
/// @brief Gets the effect-specific amount.
/// @param shader Shader to inspect.
/// @return The configured amount, or `0` for an invalid handle.
int64_t rt_shader2d_get_amount(void *shader);
/// @brief Sets the RGB color used by color-dependent effects.
/// @param shader Shader to update.
/// @param rgb Effect color encoded as `0x00RRGGBB`; higher bits are discarded.
void rt_shader2d_set_color(void *shader, int64_t rgb);
/// @brief Gets the effect color.
/// @param shader Shader to inspect.
/// @return The configured `0x00RRGGBB` color, defaulting to white.
int64_t rt_shader2d_get_color(void *shader);
/// @brief Applies the active effect to a Pixels image.
/// @param shader Shader supplying the effect; `NULL` requests a plain clone.
/// @param pixels Borrowed source Pixels handle.
/// @return A new owned Pixels handle, or `NULL` for invalid input or allocation
///         failure.
void *rt_shader2d_apply(void *shader, void *pixels);

//===----------------------------------------------------------------------===//
// PostProcess2D — a configurable full-frame post-process effect.
//===----------------------------------------------------------------------===//

/// @brief Creates a post-process effect initialized to no-op cloning.
/// @return An owned PostProcess2D handle, or `NULL` if allocation fails.
/// @details The initial amount is `1` and the effect color is white.
void *rt_postprocess2d_new(void);
/// @brief Sets the post-process effect index.
/// @param postprocess PostProcess2D to update.
/// @param effect Requested `RT_GRAPHICS2D_EFFECT_*` value, clamped to `0..4`.
void rt_postprocess2d_set_effect(void *postprocess, int64_t effect);
/// @brief Sets the post-process effect amount.
/// @param postprocess PostProcess2D to update.
/// @param amount Requested amount clamped to `0..10`.
void rt_postprocess2d_set_amount(void *postprocess, int64_t amount);
/// @brief Sets the post-process effect color.
/// @param postprocess PostProcess2D to update.
/// @param rgb Color encoded as `0x00RRGGBB`; higher bits are discarded.
void rt_postprocess2d_set_color(void *postprocess, int64_t rgb);
/// @brief Applies the configured post-process to a Pixels image.
/// @param postprocess Effect state; `NULL` requests a plain clone.
/// @param pixels Borrowed source Pixels handle.
/// @return A new owned Pixels handle, or `NULL` for invalid input or allocation
///         failure.
void *rt_postprocess2d_apply(void *postprocess, void *pixels);

//===----------------------------------------------------------------------===//
// Viewport2D — virtual-resolution to screen mapping with optional integer scale.
//===----------------------------------------------------------------------===//

/// @brief Creates a centered virtual-resolution viewport.
/// @param virtual_width Logical canvas width, normalized to a valid dimension.
/// @param virtual_height Logical canvas height, normalized to a valid dimension.
/// @param screen_width Physical output width, normalized to a valid dimension.
/// @param screen_height Physical output height, normalized to a valid dimension.
/// @return An owned Viewport2D handle, or `NULL` if allocation fails.
/// @details Fractional uniform scaling is enabled initially and any unused
///          screen area becomes centered letterboxing or pillarboxing.
void *rt_viewport2d_new(int64_t virtual_width,
                        int64_t virtual_height,
                        int64_t screen_width,
                        int64_t screen_height);
/// @brief Creates a ScreenScaler with Viewport2D behavior and distinct type identity.
/// @param virtual_width Logical canvas width, normalized to a valid dimension.
/// @param virtual_height Logical canvas height, normalized to a valid dimension.
/// @param screen_width Physical output width, normalized to a valid dimension.
/// @param screen_height Physical output height, normalized to a valid dimension.
/// @return An owned ScreenScaler handle, or `NULL` if allocation fails.
void *rt_screenscaler_new(int64_t virtual_width,
                          int64_t virtual_height,
                          int64_t screen_width,
                          int64_t screen_height);
/// @brief Sets the virtual resolution and immediately recalculates the mapping.
/// @param viewport Viewport2D or ScreenScaler to update.
/// @param width Logical width normalized to the implementation's valid range.
/// @param height Logical height normalized to the implementation's valid range.
void rt_viewport2d_set_virtual_size(void *viewport, int64_t width, int64_t height);
/// @brief Sets the screen resolution and immediately recalculates the mapping.
/// @param viewport Viewport2D or ScreenScaler to update.
/// @param width Physical screen width normalized to the valid range.
/// @param height Physical screen height normalized to the valid range.
void rt_viewport2d_set_screen_size(void *viewport, int64_t width, int64_t height);
/// @brief Enables or disables integer-only scaling.
/// @param viewport Viewport2D or ScreenScaler to update.
/// @param enabled Nonzero selects whole-number scale factors; zero allows
///        fractional factors.
/// @details The scale and centered offsets are recalculated immediately.
void rt_viewport2d_set_integer_scaling(void *viewport, int64_t enabled);
/// @brief Gets the computed fixed-point uniform scale.
/// @param viewport Viewport2D or ScreenScaler to inspect.
/// @return Thousandths of a scale unit, where `1000` is 1.0x; invalid handles
///         return `1000`.
int64_t rt_viewport2d_get_scale(void *viewport);
/// @brief Gets the horizontal centered-output offset.
/// @param viewport Viewport2D or ScreenScaler to inspect.
/// @return Letterbox or pillarbox offset in screen pixels, or `0` if invalid.
int64_t rt_viewport2d_get_offset_x(void *viewport);
/// @brief Gets the vertical centered-output offset.
/// @param viewport Viewport2D or ScreenScaler to inspect.
/// @return Letterbox or pillarbox offset in screen pixels, or `0` if invalid.
int64_t rt_viewport2d_get_offset_y(void *viewport);
/// @brief Maps a virtual-space X coordinate to screen space.
/// @param viewport Mapping to apply.
/// @param x Virtual X coordinate.
/// @return The nearest screen-space integer coordinate, or @p x if invalid.
int64_t rt_viewport2d_world_to_screen_x(void *viewport, int64_t x);
/// @brief Maps a virtual-space Y coordinate to screen space.
/// @param viewport Mapping to apply.
/// @param y Virtual Y coordinate.
/// @return The nearest screen-space integer coordinate, or @p y if invalid.
int64_t rt_viewport2d_world_to_screen_y(void *viewport, int64_t y);
/// @brief Maps a screen-space X coordinate back to virtual space.
/// @param viewport Mapping to invert.
/// @param x Screen-space X coordinate.
/// @return The nearest virtual-space integer coordinate, or @p x if invalid.
int64_t rt_viewport2d_screen_to_world_x(void *viewport, int64_t x);
/// @brief Maps a screen-space Y coordinate back to virtual space.
/// @param viewport Mapping to invert.
/// @param y Screen-space Y coordinate.
/// @return The nearest virtual-space integer coordinate, or @p y if invalid.
int64_t rt_viewport2d_screen_to_world_y(void *viewport, int64_t y);

//===----------------------------------------------------------------------===//
// Tileset2D — a grid of equally sized tiles sliced from one Pixels image.
//===----------------------------------------------------------------------===//

/// @brief Creates a uniform grid of whole tiles over a Pixels image.
/// @param pixels Valid source Pixels handle retained by the tileset.
/// @param tile_width Positive tile width in pixels.
/// @param tile_height Positive tile height in pixels.
/// @return An owned Tileset2D handle, or `NULL` when the input is invalid, the
///         image contains no complete tile, or allocation fails.
/// @details Partial rows and columns at the image's right or bottom edge are
///          excluded from the grid.
void *rt_tileset2d_new(void *pixels, int64_t tile_width, int64_t tile_height);
/// @brief Gets the number of complete tile columns.
/// @param tileset Tileset to inspect.
/// @return The whole-column count, or `0` for an invalid handle.
int64_t rt_tileset2d_columns(void *tileset);
/// @brief Gets the number of complete tile rows.
/// @param tileset Tileset to inspect.
/// @return The whole-row count, or `0` for an invalid handle.
int64_t rt_tileset2d_rows(void *tileset);
/// @brief Gets the total number of addressable tiles.
/// @param tileset Tileset to inspect.
/// @return `columns * rows`, or `0` for an invalid handle.
int64_t rt_tileset2d_tile_count(void *tileset);
/// @brief Extracts one tile into a new Pixels image.
/// @param tileset Source tileset.
/// @param tile_index Zero-based row-major tile index.
/// @return A new owned Pixels handle, or `NULL` for an invalid index, handle,
///         or allocation failure.
void *rt_tileset2d_get_tile_pixels(void *tileset, int64_t tile_index);

//===----------------------------------------------------------------------===//
// TileLayer2D — a 2D grid of tile indices with visibility/opacity.
//===----------------------------------------------------------------------===//

/// @brief Creates a visible, fully opaque, zero-filled tile layer.
/// @param width Positive layer width in cells.
/// @param height Positive layer height in cells.
/// @return An owned TileLayer2D handle, or `NULL` if allocation fails.
/// @details Invalid or overflowing dimensions raise a runtime trap.
void *rt_tilelayer2d_new(int64_t width, int64_t height);
/// @brief Gets the layer width in cells.
/// @param layer Tile layer to inspect.
/// @return The width, or `0` for an invalid handle.
int64_t rt_tilelayer2d_width(void *layer);
/// @brief Gets the layer height in cells.
/// @param layer Tile layer to inspect.
/// @return The height, or `0` for an invalid handle.
int64_t rt_tilelayer2d_height(void *layer);
/// @brief Sets the tile identifier stored at a cell.
/// @param layer Tile layer to update.
/// @param x Zero-based cell X coordinate.
/// @param y Zero-based cell Y coordinate.
/// @param tile Application-defined tile identifier; `0` conventionally means empty.
/// @details Out-of-bounds writes are ignored.
void rt_tilelayer2d_set(void *layer, int64_t x, int64_t y, int64_t tile);
/// @brief Gets the tile identifier stored at a cell.
/// @param layer Tile layer to inspect.
/// @param x Zero-based cell X coordinate.
/// @param y Zero-based cell Y coordinate.
/// @return The stored identifier, or `-1` when the handle or coordinates are invalid.
int64_t rt_tilelayer2d_get(void *layer, int64_t x, int64_t y);
/// @brief Fills every layer cell with one tile identifier.
/// @param layer Tile layer to update.
/// @param tile Identifier written to every cell.
void rt_tilelayer2d_fill(void *layer, int64_t tile);
/// @brief Clears every layer cell to the conventional empty identifier `0`.
/// @param layer Tile layer to clear.
void rt_tilelayer2d_clear(void *layer);
/// @brief Sets whether the layer participates in tilemap rendering.
/// @param layer Tile layer to update.
/// @param visible Nonzero for visible, zero for hidden.
void rt_tilelayer2d_set_visible(void *layer, int64_t visible);
/// @brief Queries whether the layer is marked visible.
/// @param layer Tile layer to inspect.
/// @return `1` when visible and `0` when hidden or invalid.
int64_t rt_tilelayer2d_is_visible(void *layer);
/// @brief Sets the layer opacity percentage.
/// @param layer Tile layer to update.
/// @param opacity Requested percentage, clamped to `0..100`.
void rt_tilelayer2d_set_opacity(void *layer, int64_t opacity);
/// @brief Gets the layer opacity percentage.
/// @param layer Tile layer to inspect.
/// @return Opacity in `0..100`, or `0` for an invalid handle.
int64_t rt_tilelayer2d_get_opacity(void *layer);

//===----------------------------------------------------------------------===//
// ObjectLayer2D — a list of typed rectangular objects (Tiled-style).
//===----------------------------------------------------------------------===//

/// @brief Creates an empty rectangular-object layer.
/// @param capacity Requested initial entry capacity, normalized to a safe
///        implementation range.
/// @return An owned ObjectLayer2D handle, or `NULL` if allocation fails.
void *rt_objectlayer2d_new(int64_t capacity);
/// @brief Appends a typed rectangle object.
/// @param layer Object layer receiving the entry.
/// @param x Rectangle X coordinate.
/// @param y Rectangle Y coordinate.
/// @param width Rectangle width; a negative width moves @p x to the opposite
///        edge and is stored as positive.
/// @param height Rectangle height; a negative height moves @p y to the opposite
///        edge and is stored as positive.
/// @param type Opaque application-defined category tag.
/// @return The zero-based entry index, or `-1` for an invalid or empty
///         rectangle, invalid handle, or capacity failure.
/// @details Capacity overflow raises a runtime trap.
int64_t rt_objectlayer2d_add_rect(
    void *layer, int64_t x, int64_t y, int64_t width, int64_t height, int64_t type);
/// @brief Gets the number of objects in the layer.
/// @param layer Object layer to inspect.
/// @return The entry count, or `0` for an invalid handle.
int64_t rt_objectlayer2d_count(void *layer);
/// @brief Removes all objects without releasing reserved storage.
/// @param layer Object layer to clear.
void rt_objectlayer2d_clear(void *layer);
/// @brief Gets an object's X coordinate.
/// @param layer Object layer to inspect.
/// @param index Zero-based entry index.
/// @return The stored X coordinate, or `0` for an invalid index or handle.
int64_t rt_objectlayer2d_get_x(void *layer, int64_t index);
/// @brief Gets an object's Y coordinate.
/// @param layer Object layer to inspect.
/// @param index Zero-based entry index.
/// @return The stored Y coordinate, or `0` for an invalid index or handle.
int64_t rt_objectlayer2d_get_y(void *layer, int64_t index);
/// @brief Gets an object's normalized width.
/// @param layer Object layer to inspect.
/// @param index Zero-based entry index.
/// @return The positive stored width, or `0` for an invalid index or handle.
int64_t rt_objectlayer2d_get_width(void *layer, int64_t index);
/// @brief Gets an object's normalized height.
/// @param layer Object layer to inspect.
/// @param index Zero-based entry index.
/// @return The positive stored height, or `0` for an invalid index or handle.
int64_t rt_objectlayer2d_get_height(void *layer, int64_t index);
/// @brief Gets an object's application-defined type tag.
/// @param layer Object layer to inspect.
/// @param index Zero-based entry index.
/// @return The stored tag, or `0` for an invalid index or handle.
int64_t rt_objectlayer2d_get_type(void *layer, int64_t index);

//===----------------------------------------------------------------------===//
// AutoTile2D — bitmask-to-tile resolution for auto-tiling terrain edges.
//===----------------------------------------------------------------------===//

/// @brief Creates a zero-initialized 16-entry auto-tile rule table.
/// @return An owned AutoTile2D handle, or `NULL` if allocation fails.
void *rt_autotile2d_new(void);
/// @brief Maps a four-bit neighbor mask to a tile identifier.
/// @param autotile Rule table to update.
/// @param mask Neighbor mask; only the low four bits are used.
/// @param tile Tile identifier stored for the selected variant.
void rt_autotile2d_set_variant(void *autotile, int64_t mask, int64_t tile);
/// @brief Resolves a neighbor bitmask to its configured tile identifier.
/// @param autotile Rule table to query.
/// @param mask Neighbor mask; only the low four bits are used.
/// @return The configured identifier, or `0` for an invalid handle or an
///         untouched variant.
int64_t rt_autotile2d_resolve(void *autotile, int64_t mask);
/// @brief Resolves a mask and writes the result into a TileLayer2D cell.
/// @param autotile Rule table to query.
/// @param layer TileLayer2D receiving the resolved identifier.
/// @param x Destination cell X coordinate.
/// @param y Destination cell Y coordinate.
/// @param mask Neighbor mask; only the low four bits are used.
/// @details Invalid handles and out-of-bounds cells are ignored.
void rt_autotile2d_apply(void *autotile, void *layer, int64_t x, int64_t y, int64_t mask);

//===----------------------------------------------------------------------===//
// Path2D — an ordered polyline that can be rasterized into pixels.
//===----------------------------------------------------------------------===//

/// @brief Creates an empty polyline path.
/// @param capacity Requested initial point capacity, normalized to the
///        implementation's safe range.
/// @return An owned Path2D handle, or `NULL` if allocation fails.
void *rt_path2d_new(int64_t capacity);
/// @brief Removes all path points while preserving reserved storage.
/// @param path Path to clear.
void rt_path2d_clear(void *path);
/// @brief Starts a disconnected subpath at a point.
/// @param path Path receiving the move point.
/// @param x New current X coordinate.
/// @param y New current Y coordinate.
/// @details No segment connects the previous point to this move point.
void rt_path2d_move_to(void *path, int64_t x, int64_t y);
/// @brief Appends a line endpoint to the current subpath.
/// @param path Path receiving the endpoint.
/// @param x Endpoint X coordinate.
/// @param y Endpoint Y coordinate.
/// @details Rasterization joins the preceding point to this endpoint.
void rt_path2d_line_to(void *path, int64_t x, int64_t y);
/// @brief Gets the number of stored move and line points.
/// @param path Path to inspect.
/// @return The point count, or `0` for an invalid handle.
int64_t rt_path2d_count(void *path);
/// @brief Gets a path point's X coordinate.
/// @param path Path to inspect.
/// @param index Zero-based point index.
/// @return The stored X coordinate, or `0` for an invalid index or handle.
int64_t rt_path2d_get_x(void *path, int64_t index);
/// @brief Gets a path point's Y coordinate.
/// @param path Path to inspect.
/// @param index Zero-based point index.
/// @return The stored Y coordinate, or `0` for an invalid index or handle.
int64_t rt_path2d_get_y(void *path, int64_t index);
/// @brief Rasterizes the path's line segments into a Pixels image.
/// @param path Path whose subpaths are traversed.
/// @param pixels Destination Pixels image, modified in place.
/// @param rgb Color accepted as `0x00RRGGBB`, `0xRRGGBBAA`, or a tagged Color
///        value; alpha is ignored by the RGB line primitive.
/// @details Move points break connectivity, and a path with fewer than two
///          points produces no drawing.
void rt_path2d_draw_to_pixels(void *path, void *pixels, int64_t rgb);

//===----------------------------------------------------------------------===//
// ShapeRenderer2D — stroked/filled primitive rasterizer.
//===----------------------------------------------------------------------===//

/// @brief Creates a shape renderer with white stroke and disabled fill.
/// @return An owned ShapeRenderer2D handle, or `NULL` if allocation fails.
void *rt_shaperenderer2d_new(void);
/// @brief Sets the outline color or disables outline drawing.
/// @param renderer Shape renderer to update.
/// @param rgb Color accepted as `0x00RRGGBB`, `0xRRGGBBAA`, or a tagged Color
///        value; any negative value disables the stroke.
/// @details Alpha is ignored by the underlying RGB raster primitives.
void rt_shaperenderer2d_set_stroke(void *renderer, int64_t rgb);
/// @brief Sets the interior color or disables fill drawing.
/// @param renderer Shape renderer to update.
/// @param rgb Color accepted in the same forms as the stroke; any negative
///        value disables the fill.
/// @details Fill alpha is ignored rather than composited.
void rt_shaperenderer2d_set_fill(void *renderer, int64_t rgb);
/// @brief Draws a line with the current stroke color.
/// @param renderer Shape renderer supplying state.
/// @param pixels Destination Pixels image.
/// @param x0 Starting X coordinate.
/// @param y0 Starting Y coordinate.
/// @param x1 Ending X coordinate.
/// @param y1 Ending Y coordinate.
/// @details A disabled stroke or invalid handle makes this a no-op.
void rt_shaperenderer2d_line(
    void *renderer, void *pixels, int64_t x0, int64_t y0, int64_t x1, int64_t y1);
/// @brief Draws a filled and/or outlined rectangle.
/// @param renderer Shape renderer supplying fill and stroke state.
/// @param pixels Destination Pixels image.
/// @param x Rectangle X coordinate.
/// @param y Rectangle Y coordinate.
/// @param width Rectangle width in pixels.
/// @param height Rectangle height in pixels.
/// @details The fill is drawn first and the one-pixel outline second.
void rt_shaperenderer2d_rect(
    void *renderer, void *pixels, int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Draws a filled disc and/or outlined circle.
/// @param renderer Shape renderer supplying fill and stroke state.
/// @param pixels Destination Pixels image.
/// @param x Circle center X coordinate.
/// @param y Circle center Y coordinate.
/// @param radius Circle radius in pixels.
/// @details The disc fill is drawn before the ring outline.
void rt_shaperenderer2d_circle(void *renderer, void *pixels, int64_t x, int64_t y, int64_t radius);
/// @brief Strokes a Path2D with the current outline color.
/// @param renderer Shape renderer supplying stroke state.
/// @param pixels Destination Pixels image.
/// @param path Path2D to rasterize.
/// @details Fill state is ignored because closed-path filling is not supported.
void rt_shaperenderer2d_path(void *renderer, void *pixels, void *path);

//===----------------------------------------------------------------------===//
// TextRenderer2D — font-based text drawing/measurement onto a canvas.
//===----------------------------------------------------------------------===//

/// @brief Creates a text renderer using the built-in font at 1x in white.
/// @return An owned TextRenderer2D handle, or `NULL` if allocation fails.
void *rt_textrenderer2d_new(void);
/// @brief Sets the bitmap font used for measurement and drawing.
/// @param renderer Text renderer to update.
/// @param font BitmapFont handle retained by the renderer, or `NULL` to restore
///        the built-in Canvas font.
/// @details Invalid non-null font handles are ignored; replacing a font
///          releases the previous retained reference.
void rt_textrenderer2d_set_font(void *renderer, void *font);
/// @brief Sets the integer text scale factor.
/// @param renderer Text renderer to update.
/// @param scale Requested scale clamped to `1..64`.
void rt_textrenderer2d_set_scale(void *renderer, int64_t scale);
/// @brief Sets the text RGB color.
/// @param renderer Text renderer to update.
/// @param rgb Color encoded as `0x00RRGGBB`; higher bits are discarded.
void rt_textrenderer2d_set_color(void *renderer, int64_t rgb);
/// @brief Measures the rendered width of a string.
/// @param renderer Text renderer supplying font and scale; an invalid handle
///        uses the unscaled built-in Canvas font.
/// @param text String to measure.
/// @return Width in pixels, using saturating multiplication for scaling.
int64_t rt_textrenderer2d_measure_width(void *renderer, rt_string text);
/// @brief Measures one rendered line's font height.
/// @param renderer Text renderer supplying font and scale; an invalid handle
///        uses the built-in Canvas font.
/// @param text Accepted for API symmetry but not inspected because line height
///        is font-uniform.
/// @return Line height in pixels, using saturating multiplication for scaling.
int64_t rt_textrenderer2d_measure_height(void *renderer, rt_string text);
/// @brief Draws a string onto a Canvas.
/// @param renderer Text renderer supplying font, scale, and color.
/// @param canvas Destination Canvas.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param text String to draw.
void rt_textrenderer2d_draw(void *renderer, void *canvas, int64_t x, int64_t y, rt_string text);

//===----------------------------------------------------------------------===//
// SDFFont — signed-distance-field font wrapper over a bitmap font.
//===----------------------------------------------------------------------===//

/// @brief Wraps an optional BitmapFont in the forward-compatible SdfFont type.
/// @param bitmap_font Valid BitmapFont retained by the wrapper, or `NULL`.
/// @param spread Requested distance-field spread clamped to `1..64`.
/// @return An owned SdfFont handle, or `NULL` for an invalid font or allocation
///         failure.
/// @details The current bitmap-backed renderer records but does not rasterize
///          using the spread value.
void *rt_sdffont_new(void *bitmap_font, int64_t spread);
/// @brief Borrows the wrapped BitmapFont.
/// @param font SdfFont to inspect.
/// @return The borrowed BitmapFont handle, or `NULL` when absent or invalid.
/// @warning The SdfFont retains ownership; callers must not release the result.
void *rt_sdffont_get_bitmap_font(void *font);
/// @brief Gets the stored SDF spread.
/// @param font SdfFont to inspect.
/// @return Spread in `1..64`, or `0` for an invalid handle.
int64_t rt_sdffont_get_spread(void *font);

//===----------------------------------------------------------------------===//
// NineSlice2D — 9-patch scalable UI image (fixed corners, stretched edges).
//===----------------------------------------------------------------------===//

/// @brief Creates a stretchable nine-slice from a Pixels image.
/// @param pixels Valid source Pixels retained by the nine-slice.
/// @param left Left border width, clamped to the image width.
/// @param top Top border height, clamped to the image height.
/// @param right Right border width, clamped to the image width.
/// @param bottom Bottom border height, clamped to the image height.
/// @return An owned NineSlice2D handle, or `NULL` for an invalid source or
///         allocation failure.
void *rt_nineslice2d_new(void *pixels, int64_t left, int64_t top, int64_t right, int64_t bottom);
/// @brief Draws the nine-slice into a destination rectangle.
/// @param slice NineSlice2D supplying the image and border insets.
/// @param target Destination Pixels image.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param width Positive destination width.
/// @param height Positive destination height.
/// @details Corners preserve their available size, edges scale along one axis,
///          and the center scales along both; borders are reduced when the
///          destination is smaller than their combined extent.
void rt_nineslice2d_draw_to_pixels(
    void *slice, void *target, int64_t x, int64_t y, int64_t width, int64_t height);

//===----------------------------------------------------------------------===//
// DebugDraw2D — queued debug overlay primitives (lines/rects/circles).
// Color parameters named "rgba" accept RGB, RGBA, or tagged Color values, but
// the raster primitives render the normalized RGB channels opaquely.
//===----------------------------------------------------------------------===//

/// @brief Creates an empty retained debug-primitive queue.
/// @param capacity Requested initial command capacity, normalized to a safe range.
/// @return An owned DebugDraw2D handle, or `NULL` if allocation fails.
void *rt_debugdraw2d_new(int64_t capacity);
/// @brief Clears all queued debug primitives while preserving reserved storage.
/// @param debug_draw Debug queue to clear.
void rt_debugdraw2d_clear(void *debug_draw);
/// @brief Gets the number of queued debug primitives.
/// @param debug_draw Debug queue to inspect.
/// @return The command count, or `0` for an invalid handle.
int64_t rt_debugdraw2d_count(void *debug_draw);
/// @brief Queues a debug line.
/// @param debug_draw Queue receiving the command.
/// @param x0 Starting X coordinate.
/// @param y0 Starting Y coordinate.
/// @param x1 Ending X coordinate.
/// @param y1 Ending Y coordinate.
/// @param rgba Color accepted as RGB, RGBA, or a tagged Color; alpha is ignored.
/// @details Command-buffer growth failure raises a runtime trap.
void rt_debugdraw2d_line(
    void *debug_draw, int64_t x0, int64_t y0, int64_t x1, int64_t y1, int64_t rgba);
/// @brief Queues a debug rectangle outline.
/// @param debug_draw Queue receiving the command.
/// @param x Rectangle X coordinate.
/// @param y Rectangle Y coordinate.
/// @param width Rectangle width.
/// @param height Rectangle height.
/// @param rgba Color accepted as RGB, RGBA, or a tagged Color; alpha is ignored.
void rt_debugdraw2d_rect(
    void *debug_draw, int64_t x, int64_t y, int64_t width, int64_t height, int64_t rgba);
/// @brief Queues a debug circle outline.
/// @param debug_draw Queue receiving the command.
/// @param x Circle center X coordinate.
/// @param y Circle center Y coordinate.
/// @param radius Circle radius.
/// @param rgba Color accepted as RGB, RGBA, or a tagged Color; alpha is ignored.
void rt_debugdraw2d_circle(void *debug_draw, int64_t x, int64_t y, int64_t radius, int64_t rgba);
/// @brief Rasterizes all queued debug primitives into a Pixels image.
/// @param debug_draw Queue whose commands are replayed in insertion order.
/// @param pixels Destination Pixels image.
/// @details Drawing does not clear the queue, allowing it to be replayed.
void rt_debugdraw2d_draw_to_pixels(void *debug_draw, void *pixels);

//===----------------------------------------------------------------------===//
// Transform2D — 2D affine transform (position/scale/rotation/origin).
//===----------------------------------------------------------------------===//

/// @brief Creates an identity 2D affine transform.
/// @return An owned Transform2D handle, or `NULL` if allocation fails.
/// @details Position, rotation, and origin begin at zero; both scale components
///          begin at `100`, representing 1.0x.
void *rt_transform2d_new(void);
/// @brief Gets the world-space X translation.
/// @param transform Transform to inspect.
/// @return The X translation, or `0` for an invalid handle.
int64_t rt_transform2d_get_x(void *transform);
/// @brief Sets the world-space X translation.
/// @param transform Transform to update.
/// @param x New X translation.
void rt_transform2d_set_x(void *transform, int64_t x);
/// @brief Gets the world-space Y translation.
/// @param transform Transform to inspect.
/// @return The Y translation, or `0` for an invalid handle.
int64_t rt_transform2d_get_y(void *transform);
/// @brief Sets the world-space Y translation.
/// @param transform Transform to update.
/// @param y New Y translation.
void rt_transform2d_set_y(void *transform, int64_t y);
/// @brief Gets the fixed-point X scale.
/// @param transform Transform to inspect.
/// @return Scale where `100` is 1.0x, defaulting to `100` if invalid.
int64_t rt_transform2d_get_scale_x(void *transform);
/// @brief Sets the fixed-point X scale.
/// @param transform Transform to update.
/// @param scale_x Requested scale clamped to `1..10000` (0.01x through 100x).
void rt_transform2d_set_scale_x(void *transform, int64_t scale_x);
/// @brief Gets the fixed-point Y scale.
/// @param transform Transform to inspect.
/// @return Scale where `100` is 1.0x, defaulting to `100` if invalid.
int64_t rt_transform2d_get_scale_y(void *transform);
/// @brief Sets the fixed-point Y scale.
/// @param transform Transform to update.
/// @param scale_y Requested scale clamped to `1..10000` (0.01x through 100x).
void rt_transform2d_set_scale_y(void *transform, int64_t scale_y);
/// @brief Gets the unnormalized integer rotation.
/// @param transform Transform to inspect.
/// @return Rotation in degrees, or `0` for an invalid handle.
int64_t rt_transform2d_get_rotation(void *transform);
/// @brief Sets the integer rotation without normalizing it.
/// @param transform Transform to update.
/// @param degrees Rotation in degrees.
/// @details The original value remains observable through `get_rotation`;
///          point transformation reduces it modulo 360 before trigonometry.
void rt_transform2d_set_rotation(void *transform, int64_t degrees);
/// @brief Sets both translation components.
/// @param transform Transform to update.
/// @param x New X translation.
/// @param y New Y translation.
void rt_transform2d_set_position(void *transform, int64_t x, int64_t y);
/// @brief Sets both fixed-point scale components.
/// @param transform Transform to update.
/// @param scale_x Requested X scale clamped to `1..10000`.
/// @param scale_y Requested Y scale clamped to `1..10000`.
void rt_transform2d_set_scale(void *transform, int64_t scale_x, int64_t scale_y);
/// @brief Sets the local origin used for scaling and rotation.
/// @param transform Transform to update.
/// @param x Origin X coordinate.
/// @param y Origin Y coordinate.
void rt_transform2d_set_origin(void *transform, int64_t x, int64_t y);
/// @brief Adds a displacement to the current translation.
/// @param transform Transform to update.
/// @param dx X displacement.
/// @param dy Y displacement.
/// @details Both additions saturate at the `int64_t` limits.
void rt_transform2d_translate(void *transform, int64_t dx, int64_t dy);
/// @brief Applies scale, rotation, origin, and translation and returns X.
/// @param transform Transform to apply.
/// @param x Input point X coordinate.
/// @param y Input point Y coordinate.
/// @return The rounded, saturating transformed X coordinate, or @p x if invalid.
/// @details Identity scale with zero-modulo rotation stays in exact integer
///          arithmetic, including coordinates near the signed 64-bit limits.
int64_t rt_transform2d_transform_x(void *transform, int64_t x, int64_t y);
/// @brief Applies scale, rotation, origin, and translation and returns Y.
/// @param transform Transform to apply.
/// @param x Input point X coordinate.
/// @param y Input point Y coordinate.
/// @return The rounded, saturating transformed Y coordinate, or @p y if invalid.
int64_t rt_transform2d_transform_y(void *transform, int64_t x, int64_t y);

//===----------------------------------------------------------------------===//
// Sampler2D — reusable filter/wrap state applied to a texture.
//===----------------------------------------------------------------------===//

/// @brief Creates a sampler using nearest filtering and clamp wrapping.
/// @return An owned Sampler2D handle, or `NULL` if allocation fails.
void *rt_sampler2d_new(void);
/// @brief Sets the sampler's texture filter.
/// @param sampler Sampler to update.
/// @param filter Linear selects bilinear filtering; every other value selects nearest.
void rt_sampler2d_set_filter(void *sampler, int64_t filter);
/// @brief Gets the sampler's texture filter.
/// @param sampler Sampler to inspect.
/// @return A `RT_GRAPHICS2D_FILTER_*` value, defaulting to nearest.
int64_t rt_sampler2d_get_filter(void *sampler);
/// @brief Sets the sampler's coordinate wrap mode.
/// @param sampler Sampler to update.
/// @param wrap Repeat selects periodic coordinates; every other value selects clamp.
void rt_sampler2d_set_wrap(void *sampler, int64_t wrap);
/// @brief Gets the sampler's coordinate wrap mode.
/// @param sampler Sampler to inspect.
/// @return A `RT_GRAPHICS2D_WRAP_*` value, defaulting to clamp.
int64_t rt_sampler2d_get_wrap(void *sampler);
/// @brief Copies the sampler's filter and wrap state onto a texture.
/// @param sampler Source Sampler2D.
/// @param texture Destination Texture2D or GpuTexture2D.
void rt_sampler2d_apply_to_texture(void *sampler, void *texture);

//===----------------------------------------------------------------------===//
// BlendState2D — reusable blend/tint/alpha state applied to a renderer.
//===----------------------------------------------------------------------===//

/// @brief Creates an alpha blend state with no tint and full opacity.
/// @return An owned BlendState2D handle, or `NULL` if allocation fails.
void *rt_blendstate2d_new(void);
/// @brief Sets the blend mode.
/// @param state Blend state to update.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` value clamped to `0..2`.
void rt_blendstate2d_set_blend_mode(void *state, int64_t blend_mode);
/// @brief Gets the blend mode.
/// @param state Blend state to inspect.
/// @return A `RT_GRAPHICS2D_BLEND_*` value, defaulting to alpha blending.
int64_t rt_blendstate2d_get_blend_mode(void *state);
/// @brief Sets or disables the multiplicative tint.
/// @param state Blend state to update.
/// @param rgb Tint encoded as `0x00RRGGBB`; a negative value disables tinting.
void rt_blendstate2d_set_tint(void *state, int64_t rgb);
/// @brief Gets the multiplicative tint.
/// @param state Blend state to inspect.
/// @return The `0x00RRGGBB` tint, or `-1` when disabled or invalid.
int64_t rt_blendstate2d_get_tint(void *state);
/// @brief Sets the global alpha scale.
/// @param state Blend state to update.
/// @param alpha Requested alpha clamped to `0..255`.
void rt_blendstate2d_set_alpha(void *state, int64_t alpha);
/// @brief Gets the global alpha scale.
/// @param state Blend state to inspect.
/// @return Alpha in `0..255`, defaulting to `255` if invalid.
int64_t rt_blendstate2d_get_alpha(void *state);
/// @brief Copies blend, tint, and alpha state onto a Renderer2D.
/// @param state Source BlendState2D.
/// @param renderer Destination Renderer2D whose current state is overwritten.
void rt_blendstate2d_apply_to_renderer(void *state, void *renderer);

//===----------------------------------------------------------------------===//
// SpriteRenderer2D — draws sprites through a material/sampler/blend pipeline.
//===----------------------------------------------------------------------===//

/// @brief Creates a sprite-pipeline wrapper with no overrides bound.
/// @return An owned SpriteRenderer2D handle, or `NULL` if allocation fails.
void *rt_spriterenderer2d_new(void);
/// @brief Sets the retained material override.
/// @param sprite_renderer Sprite renderer to update.
/// @param material Valid Material2D retained by the renderer, or `NULL` to clear.
/// @details The material supplies tint, alpha, and blend mode for each draw.
void rt_spriterenderer2d_set_material(void *sprite_renderer, void *material);
/// @brief Sets the retained texture-sampler override.
/// @param sprite_renderer Sprite renderer to update.
/// @param sampler Valid Sampler2D retained by the renderer, or `NULL` to clear.
void rt_spriterenderer2d_set_sampler(void *sprite_renderer, void *sampler);
/// @brief Sets the retained blend-state override.
/// @param sprite_renderer Sprite renderer to update.
/// @param blend_state Valid BlendState2D retained by the renderer, or `NULL` to clear.
/// @details Blend-state values override overlapping material values.
void rt_spriterenderer2d_set_blend_state(void *sprite_renderer, void *blend_state);
/// @brief Queues a raw Pixels draw through the configured state pipeline.
/// @param sprite_renderer Pipeline state provider.
/// @param renderer Active Renderer2D receiving the command.
/// @param pixels Pixels source passed to the renderer.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @details Pipeline state is applied only while the command is queued; the
///          Renderer2D's prior current state is restored afterward.
void rt_spriterenderer2d_draw_pixels(
    void *sprite_renderer, void *renderer, void *pixels, int64_t x, int64_t y);
/// @brief Queues a Texture2D draw through the configured state pipeline.
/// @param sprite_renderer Pipeline state provider.
/// @param renderer Active Renderer2D receiving the command.
/// @param texture Texture2D source passed to the renderer.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @details A bound Sampler2D overrides the texture's filter and wrap state;
///          renderer state is restored after the command is captured.
void rt_spriterenderer2d_draw_texture(
    void *sprite_renderer, void *renderer, void *texture, int64_t x, int64_t y);

//===----------------------------------------------------------------------===//
// TileChunkCache2D — dirty-tracked chunk cache for large tilemap redraws.
//===----------------------------------------------------------------------===//

/// @brief Creates a dirty-notification counter with normalized chunk dimensions.
/// @param chunk_width Requested chunk width in tiles, normalized to a valid dimension.
/// @param chunk_height Requested chunk height in tiles, normalized to a valid dimension.
/// @return An owned TileChunkCache2D handle, or `NULL` if allocation fails.
void *rt_tilechunkcache2d_new(int64_t chunk_width, int64_t chunk_height);
/// @brief Gets the configured chunk width.
/// @param cache Chunk cache to inspect.
/// @return Width in tiles, or `0` for an invalid handle.
int64_t rt_tilechunkcache2d_get_chunk_width(void *cache);
/// @brief Gets the configured chunk height.
/// @param cache Chunk cache to inspect.
/// @return Height in tiles, or `0` for an invalid handle.
int64_t rt_tilechunkcache2d_get_chunk_height(void *cache);
/// @brief Records one additional dirty-chunk notification.
/// @param cache Chunk cache to update.
/// @details The saturating counter does not identify individual chunks.
void rt_tilechunkcache2d_mark_dirty(void *cache);
/// @brief Resets the dirty-notification count to zero.
/// @param cache Chunk cache to update.
void rt_tilechunkcache2d_clear_dirty(void *cache);
/// @brief Gets the pending dirty-notification count.
/// @param cache Chunk cache to inspect.
/// @return The saturating count, or `0` for an invalid handle.
int64_t rt_tilechunkcache2d_get_dirty_count(void *cache);

//===----------------------------------------------------------------------===//
// TileMapRenderer2D — draws a tilemap (optionally chunk-cached) to a canvas.
//===----------------------------------------------------------------------===//

/// @brief Creates a tilemap renderer with no chunk cache and zero draw count.
/// @return An owned TileMapRenderer2D handle, or `NULL` if allocation fails.
void *rt_tilemaprenderer2d_new(void);
/// @brief Sets the retained chunk-cache association.
/// @param renderer Tilemap renderer to update.
/// @param cache Valid TileChunkCache2D retained by the renderer, or `NULL` to clear.
void rt_tilemaprenderer2d_set_chunk_cache(void *renderer, void *cache);
/// @brief Gets the number of visible non-empty tiles counted by the last draw.
/// @param renderer Tilemap renderer to inspect.
/// @return The most recent draw count, or `0` for an invalid handle.
int64_t rt_tilemaprenderer2d_get_draw_count(void *renderer);
/// @brief Draws an entire tilemap onto a Canvas.
/// @param renderer Tilemap renderer recording draw statistics.
/// @param tilemap Tilemap handle to draw.
/// @param canvas Destination Canvas.
/// @param offset_x X offset added to tilemap drawing coordinates.
/// @param offset_y Y offset added to tilemap drawing coordinates.
/// @details The renderer records the number of visible non-empty tiles before
///          delegating to the Tilemap drawing API.
void rt_tilemaprenderer2d_draw(
    void *renderer, void *tilemap, void *canvas, int64_t offset_x, int64_t offset_y);
/// @brief Draws only tiles intersecting a view rectangle.
/// @param renderer Tilemap renderer recording draw statistics.
/// @param tilemap Tilemap handle to draw.
/// @param canvas Destination Canvas.
/// @param offset_x X offset applied while drawing.
/// @param offset_y Y offset applied while drawing.
/// @param view_x View rectangle X coordinate in tilemap drawing space.
/// @param view_y View rectangle Y coordinate in tilemap drawing space.
/// @param view_w View rectangle width.
/// @param view_h View rectangle height.
/// @details The recorded count reflects non-empty tiles selected by the same
///          culling rectangle.
void rt_tilemaprenderer2d_draw_region(void *renderer,
                                      void *tilemap,
                                      void *canvas,
                                      int64_t offset_x,
                                      int64_t offset_y,
                                      int64_t view_x,
                                      int64_t view_y,
                                      int64_t view_w,
                                      int64_t view_h);

//===----------------------------------------------------------------------===//
// AnimationClip2D / AnimatedSprite2D — frame-range sprite animation.
//===----------------------------------------------------------------------===//

/// @brief Creates an immutable description of a contiguous animation range.
/// @param start_frame First sprite-sheet frame; negative values become `0`.
/// @param frame_count Number of frames; non-positive values become `1`.
/// @param frame_delay_ms Delay per frame; non-positive values become `100` ms.
/// @param loop Nonzero to wrap after the final effective frame.
/// @return An owned AnimationClip2D handle, or `NULL` if allocation fails.
void *rt_animationclip2d_new(int64_t start_frame,
                             int64_t frame_count,
                             int64_t frame_delay_ms,
                             int64_t loop);
/// @brief Gets the clip's first sprite-sheet frame index.
/// @param clip Animation clip to inspect.
/// @return The normalized starting frame, or `0` for an invalid handle.
int64_t rt_animationclip2d_get_start_frame(void *clip);
/// @brief Gets the requested number of clip frames.
/// @param clip Animation clip to inspect.
/// @return The positive frame count, or `0` for an invalid handle.
int64_t rt_animationclip2d_get_frame_count(void *clip);
/// @brief Gets the per-frame delay.
/// @param clip Animation clip to inspect.
/// @return Positive delay in milliseconds, or `0` for an invalid handle.
int64_t rt_animationclip2d_get_frame_delay_ms(void *clip);
/// @brief Queries whether the clip loops.
/// @param clip Animation clip to inspect.
/// @return `1` for looping and `0` for non-looping or invalid.
int64_t rt_animationclip2d_get_loop(void *clip);

/// @brief Creates an animation controller around a Sprite.
/// @param sprite Valid Sprite retained by the controller.
/// @return An owned AnimatedSprite2D handle, or `NULL` for an invalid sprite
///         or allocation failure.
/// @details The controller begins stopped with no clip.
void *rt_animatedsprite2d_new(void *sprite);
/// @brief Assigns a retained clip and resets animation state.
/// @param animated_sprite Controller to update.
/// @param clip Valid AnimationClip2D retained by the controller, or `NULL` to
///        remove the clip and stop playback.
/// @details A non-null clip begins playing immediately and seeks the wrapped
///          Sprite to the clip's starting frame.
void rt_animatedsprite2d_set_clip(void *animated_sprite, void *clip);
/// @brief Starts or resumes playback of the assigned clip.
/// @param animated_sprite Controller to play.
/// @details Playback remains stopped if the clip has no frames available in
///          the wrapped Sprite.  Replaying a completed clip restarts it.
void rt_animatedsprite2d_play(void *animated_sprite);
/// @brief Stops playback and resets to the clip's first frame.
/// @param animated_sprite Controller to stop.
void rt_animatedsprite2d_stop(void *animated_sprite);
/// @brief Advances playback by an elapsed duration.
/// @param animated_sprite Controller to update.
/// @param delta_ms Elapsed milliseconds; negative values are treated as zero.
/// @details Large deltas can advance multiple frames.  Frame availability is
///          capped by the wrapped Sprite; looping wraps, while a non-looping
///          clip stops on its last effective frame.
void rt_animatedsprite2d_update(void *animated_sprite, int64_t delta_ms);
/// @brief Gets the current clip-local frame index.
/// @param animated_sprite Controller to inspect.
/// @return Zero-based offset within the clip, or `0` for an invalid handle.
/// @note This is not the absolute Sprite sheet frame; add the clip start frame.
int64_t rt_animatedsprite2d_get_frame(void *animated_sprite);
/// @brief Queries whether a valid sprite and clip are actively playing.
/// @param animated_sprite Controller to inspect.
/// @return `1` when playing, otherwise `0`.
int64_t rt_animatedsprite2d_is_playing(void *animated_sprite);

//===----------------------------------------------------------------------===//
// TextLayout2D — wrapped/aligned multi-line text measurement.
//===----------------------------------------------------------------------===//

/// @brief Creates a text layout using the built-in font and default settings.
/// @return An owned TextLayout2D handle, or `NULL` if allocation fails.
/// @details Defaults are 1x scale, white, no wrapping, and left alignment.
void *rt_textlayout2d_new(void);
/// @brief Sets the retained bitmap font used for measurement.
/// @param layout Text layout to update.
/// @param font Valid BitmapFont retained by the layout, or `NULL` for the
///        built-in Canvas font.
void rt_textlayout2d_set_font(void *layout, void *font);
/// @brief Sets the integer measurement scale.
/// @param layout Text layout to update.
/// @param scale Requested scale clamped to `1..64`.
void rt_textlayout2d_set_scale(void *layout, int64_t scale);
/// @brief Sets the word-wrap width.
/// @param layout Text layout to update.
/// @param width Width in pixels; negative values become `0`, which disables wrapping.
void rt_textlayout2d_set_wrap_width(void *layout, int64_t width);
/// @brief Sets the stored horizontal alignment mode.
/// @param layout Text layout to update.
/// @param alignment `0` for left, `1` for center, or `2` for right; values are clamped.
/// @note Alignment is presentation state and does not change width or height measurements.
void rt_textlayout2d_set_alignment(void *layout, int64_t alignment);
/// @brief Sets the stored text color.
/// @param layout Text layout to update.
/// @param rgb Color encoded as `0x00RRGGBB`; higher bits are discarded.
/// @note Color does not affect measurement.
void rt_textlayout2d_set_color(void *layout, int64_t rgb);
/// @brief Measures the maximum laid-out line width.
/// @param layout Layout supplying font, scale, and wrap width; an invalid
///        handle uses default font and scale without wrapping.
/// @param text Text to measure; explicit newline sequences start new lines.
/// @return Maximum line width in pixels, with long words split across the
///         configured wrap width.
int64_t rt_textlayout2d_measure_width(void *layout, rt_string text);
/// @brief Measures total laid-out text height after wrapping.
/// @param layout Layout supplying font, scale, and wrap width.
/// @param text Text to measure.
/// @return Saturating product of the font line height and the computed line count.
int64_t rt_textlayout2d_measure_height(void *layout, rt_string text);

//===----------------------------------------------------------------------===//
// RenderPass2D / RenderGraph2D — composable post-process pass pipeline.
//===----------------------------------------------------------------------===//

/// @brief Creates an enabled pass between two render surfaces.
/// @param source Valid RenderTarget2D or Surface2D retained as input.
/// @param target Valid RenderTarget2D or Surface2D retained as output.
/// @return An owned RenderPass2D handle, or `NULL` for invalid surfaces or
///         allocation failure.
void *rt_renderpass2d_new(void *source, void *target);
/// @brief Replaces the retained input surface.
/// @param pass Render pass to update.
/// @param source Valid render surface to retain, or `NULL` to disconnect input.
void rt_renderpass2d_set_source(void *pass, void *source);
/// @brief Replaces the retained output surface.
/// @param pass Render pass to update.
/// @param target Valid render surface to retain, or `NULL` to disconnect output.
void rt_renderpass2d_set_target(void *pass, void *target);
/// @brief Sets the retained effect applied during the pass.
/// @param pass Render pass to update.
/// @param shader Valid Shader2D or PostProcess2D to retain, or `NULL` for a clone.
void rt_renderpass2d_set_shader(void *pass, void *shader);
/// @brief Enables or disables pass execution.
/// @param pass Render pass to update.
/// @param enabled Nonzero to enable, zero to disable.
void rt_renderpass2d_set_enabled(void *pass, int64_t enabled);
/// @brief Queries whether the pass is enabled.
/// @param pass Render pass to inspect.
/// @return `1` when enabled, otherwise `0`.
int64_t rt_renderpass2d_get_enabled(void *pass);
/// @brief Executes the source-to-effect-to-target pipeline.
/// @param pass Render pass to execute.
/// @details Disabled or disconnected passes do nothing.  The source is cloned
///          or effect-processed, the target is cleared, and the result is
///          alpha-composited at its origin.
void rt_renderpass2d_execute(void *pass);

/// @brief Creates an empty ordered render-pass graph.
/// @param capacity Requested initial pass capacity, normalized to a safe range.
/// @return An owned RenderGraph2D handle, or `NULL` if allocation fails.
void *rt_rendergraph2d_new(int64_t capacity);
/// @brief Appends and retains a render pass.
/// @param graph Render graph receiving the pass.
/// @param pass Valid RenderPass2D to retain.
/// @details Capacity failure raises a runtime trap and does not append the pass.
void rt_rendergraph2d_add_pass(void *graph, void *pass);
/// @brief Removes all passes and releases their retained references.
/// @param graph Render graph to clear.
void rt_rendergraph2d_clear(void *graph);
/// @brief Gets the number of passes in the graph.
/// @param graph Render graph to inspect.
/// @return The pass count, or `0` for an invalid handle.
int64_t rt_rendergraph2d_get_count(void *graph);
/// @brief Executes every pass in insertion order.
/// @param graph Render graph to execute.
/// @details Individual disabled passes skip themselves.
void rt_rendergraph2d_execute(void *graph);

//===----------------------------------------------------------------------===//
// CollisionMask2D / HitBox2D — pixel-mask and AABB collision helpers.
//===----------------------------------------------------------------------===//

/// @brief Creates an all-clear collision mask.
/// @param width Positive mask width in cells.
/// @param height Positive mask height in cells.
/// @return An owned CollisionMask2D handle, or `NULL` if allocation fails.
/// @details Invalid or overflowing dimensions raise a runtime trap.
void *rt_collisionmask2d_new(int64_t width, int64_t height);
/// @brief Builds a collision mask from image alpha.
/// @param pixels Valid source Pixels image.
/// @param alpha_threshold Threshold clamped to `0..255`; zero marks every
///        nontransparent pixel solid, while positive values use `alpha >= threshold`.
/// @return A new owned CollisionMask2D handle, or `NULL` for invalid input or
///         allocation failure.
void *rt_collisionmask2d_from_pixels(void *pixels, int64_t alpha_threshold);
/// @brief Gets the mask width.
/// @param mask Collision mask to inspect.
/// @return Width in cells, or `0` for an invalid handle.
int64_t rt_collisionmask2d_get_width(void *mask);
/// @brief Gets the mask height.
/// @param mask Collision mask to inspect.
/// @return Height in cells, or `0` for an invalid handle.
int64_t rt_collisionmask2d_get_height(void *mask);
/// @brief Sets or clears one mask cell.
/// @param mask Collision mask to update.
/// @param x Zero-based cell X coordinate.
/// @param y Zero-based cell Y coordinate.
/// @param solid Nonzero to mark solid, zero to clear.
/// @details Out-of-bounds writes are ignored.
void rt_collisionmask2d_set(void *mask, int64_t x, int64_t y, int64_t solid);
/// @brief Queries whether one mask cell is solid.
/// @param mask Collision mask to inspect.
/// @param x Zero-based cell X coordinate.
/// @param y Zero-based cell Y coordinate.
/// @return `1` when solid, or `0` when clear, out of bounds, or invalid.
int64_t rt_collisionmask2d_get(void *mask, int64_t x, int64_t y);
/// @brief Tests pixel-perfect overlap between two positioned masks.
/// @param a First collision mask.
/// @param ax World X coordinate of @p a's top-left cell.
/// @param ay World Y coordinate of @p a's top-left cell.
/// @param b Second collision mask.
/// @param bx World X coordinate of @p b's top-left cell.
/// @param by World Y coordinate of @p b's top-left cell.
/// @return `1` if any intersecting world cell is solid in both masks, otherwise `0`.
int64_t rt_collisionmask2d_overlaps(
    void *a, int64_t ax, int64_t ay, void *b, int64_t bx, int64_t by);

/// @brief Creates an axis-aligned half-open hit box.
/// @param x Top-left world X coordinate.
/// @param y Top-left world Y coordinate.
/// @param width Width; negative values are clamped to zero.
/// @param height Height; negative values are clamped to zero.
/// @return An owned HitBox2D handle, or `NULL` if allocation fails.
void *rt_hitbox2d_new(int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Replaces the hit box rectangle.
/// @param hitbox Hit box to update.
/// @param x New top-left X coordinate.
/// @param y New top-left Y coordinate.
/// @param width New width, with negative values clamped to zero.
/// @param height New height, with negative values clamped to zero.
void rt_hitbox2d_set(void *hitbox, int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Gets the hit box's top-left X coordinate.
/// @param hitbox Hit box to inspect.
/// @return The X coordinate, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_x(void *hitbox);
/// @brief Gets the hit box's top-left Y coordinate.
/// @param hitbox Hit box to inspect.
/// @return The Y coordinate, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_y(void *hitbox);
/// @brief Gets the nonnegative hit box width.
/// @param hitbox Hit box to inspect.
/// @return Width, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_width(void *hitbox);
/// @brief Gets the nonnegative hit box height.
/// @param hitbox Hit box to inspect.
/// @return Height, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_height(void *hitbox);
/// @brief Tests whether a point lies inside the hit box.
/// @param hitbox Hit box to test.
/// @param x World-space point X coordinate.
/// @param y World-space point Y coordinate.
/// @return `1` for membership in `[x, x+width) x [y, y+height)`, otherwise `0`.
int64_t rt_hitbox2d_contains(void *hitbox, int64_t x, int64_t y);
/// @brief Tests whether two hit boxes have positive-area overlap.
/// @param a First HitBox2D.
/// @param b Second HitBox2D.
/// @return `1` on overlap, otherwise `0`; merely touching edges do not intersect.
int64_t rt_hitbox2d_intersects(void *a, void *b);

//===----------------------------------------------------------------------===//
// Palette2D / Gradient2D — indexed color and color-ramp helpers.
//===----------------------------------------------------------------------===//

/// @brief Creates an empty fixed-capacity 256-color palette.
/// @return An owned Palette2D handle, or `NULL` if allocation fails.
void *rt_palette2d_new(void);
/// @brief Defines or replaces one palette slot.
/// @param palette Palette to update.
/// @param index Slot in `0..255`; out-of-range writes are ignored.
/// @param rgba Raw `0xRRGGBBAA` or tagged Color value normalized to raw RGBA.
/// @details The reported count becomes one greater than the highest index ever set.
void rt_palette2d_set_color(void *palette, int64_t index, int64_t rgba);
/// @brief Gets a raw palette color.
/// @param palette Palette to inspect.
/// @param index Slot in `0..255`.
/// @return Raw `0xRRGGBBAA`, or `0` for an undefined slot or invalid input.
int64_t rt_palette2d_get_color(void *palette, int64_t index);
/// @brief Gets a palette color in tagged Color representation.
/// @param palette Palette to inspect.
/// @param index Slot in `0..255`.
/// @return A Color.RGBA-compatible value converted from the raw slot result.
int64_t rt_palette2d_get_color_value(void *palette, int64_t index);
/// @brief Gets the palette's logical extent.
/// @param palette Palette to inspect.
/// @return One greater than the highest defined slot, or `0` if empty or invalid.
/// @note Undefined gaps below the count do not themselves become defined.
int64_t rt_palette2d_get_count(void *palette);
/// @brief Maps each source pixel to its nearest defined palette color.
/// @param palette Palette supplying candidate colors.
/// @param pixels Borrowed source Pixels image.
/// @return A new owned Pixels image, or `NULL` for invalid input or allocation failure.
/// @details Distance is squared Euclidean distance across RGBA channels.  An
///          empty palette returns a distinct exact clone of the source.
void *rt_palette2d_apply(void *palette, void *pixels);
/// @brief Applies the legacy red-byte indexed palette mapping.
/// @param palette Palette supplying indexed replacement colors.
/// @param pixels Borrowed source Pixels image.
/// @return A new owned Pixels image, or `NULL` for invalid input or allocation failure.
/// @details The red byte normally selects the slot; legacy `0x000000II` pixels
///          instead use their alpha byte.  Undefined slots preserve the source pixel.
void *rt_palette2d_apply_legacy(void *palette, void *pixels);

/// @brief Creates an RGBA gradient between two endpoints.
/// @param start_rgba Raw RGBA or tagged Color start value.
/// @param end_rgba Raw RGBA or tagged Color end value.
/// @param steps Quantization level count; non-positive values become `2`.
/// @return An owned Gradient2D handle, or `NULL` if allocation fails.
void *rt_gradient2d_new(int64_t start_rgba, int64_t end_rgba, int64_t steps);
/// @brief Replaces both normalized gradient endpoint colors.
/// @param gradient Gradient to update.
/// @param start_rgba Raw RGBA or tagged Color start value.
/// @param end_rgba Raw RGBA or tagged Color end value.
void rt_gradient2d_set_colors(void *gradient, int64_t start_rgba, int64_t end_rgba);
/// @brief Sets the number of quantization levels.
/// @param gradient Gradient to update.
/// @param steps Level count; non-positive values reset to `2`.
/// @details Counts above `101` are treated as `101` while sampling.
void rt_gradient2d_set_steps(void *gradient, int64_t steps);
/// @brief Samples the gradient at an integer percentage.
/// @param gradient Gradient to sample.
/// @param t_pct Position clamped to `0..100`.
/// @return Interpolated raw `0xRRGGBBAA`, or `0` for an invalid handle.
/// @details Two or fewer steps produce smooth channel interpolation; larger
///          counts quantize the percentage into evenly spaced levels.
int64_t rt_gradient2d_sample(void *gradient, int64_t t_pct);
/// @brief Samples an integer percentage in tagged Color representation.
/// @param gradient Gradient to sample.
/// @param t_pct Position clamped to `0..100`.
/// @return A Color.RGBA-compatible form of @ref rt_gradient2d_sample.
int64_t rt_gradient2d_sample_color(void *gradient, int64_t t_pct);
/// @brief Samples the gradient from a floating-point position.
/// @param gradient Gradient to sample.
/// @param t Normalized position in `0.0..1.0`; values above `1.0` are accepted
///        as legacy percentages and all values are clamped to the endpoints.
/// @return Interpolated raw `0xRRGGBBAA`.
int64_t rt_gradient2d_sample_normalized(void *gradient, double t);
/// @brief Samples a floating-point position in tagged Color representation.
/// @param gradient Gradient to sample.
/// @param t Normalized position or legacy percentage.
/// @return A Color.RGBA-compatible form of @ref rt_gradient2d_sample_normalized.
int64_t rt_gradient2d_sample_color_normalized(void *gradient, double t);
/// @brief Fills a Pixels image with a left-to-right gradient.
/// @param gradient Gradient supplying sampled colors.
/// @param pixels Destination Pixels image modified in place.
/// @details The first and last columns use the two endpoints when width exceeds
///          one. Content mutation generation advances once only if any texel changes.
void rt_gradient2d_fill_horizontal(void *gradient, void *pixels);
/// @brief Fills a Pixels image with a top-to-bottom gradient.
/// @param gradient Gradient supplying sampled colors.
/// @param pixels Destination Pixels image modified in place.
/// @details The first and last rows use the two endpoints when height exceeds
///          one. Content mutation generation advances once only if any texel changes.
void rt_gradient2d_fill_vertical(void *gradient, void *pixels);

//===----------------------------------------------------------------------===//
// CameraRig2D — smoothed/dead-zoned camera follow with screen shake.
//===----------------------------------------------------------------------===//

/// @brief Creates a follow-and-shake rig around an optional Camera2D.
/// @param camera Valid Camera2D retained by the rig, or `NULL`.
/// @return An owned CameraRig2D handle, or `NULL` for an invalid camera or
///         allocation failure.
/// @details Smoothing defaults to 100 percent, so the first update snaps to
///          the target; target and shake offsets begin at zero.
void *rt_camerarig2d_new(void *camera);
/// @brief Replaces the retained underlying camera.
/// @param rig Camera rig to update.
/// @param camera Valid Camera2D to retain, or `NULL` to disconnect the rig.
void rt_camerarig2d_set_camera(void *rig, void *camera);
/// @brief Sets the world-space follow target.
/// @param rig Camera rig to update.
/// @param x Target X coordinate.
/// @param y Target Y coordinate.
void rt_camerarig2d_set_target(void *rig, int64_t x, int64_t y);
/// @brief Sets the per-update follow strength.
/// @param rig Camera rig to update.
/// @param lerp_pct Requested percentage clamped to `0..100`; zero prevents
///        movement and 100 snaps immediately.
void rt_camerarig2d_set_smoothing(void *rig, int64_t lerp_pct);
/// @brief Sets the underlying camera's world-space dead-zone dimensions.
/// @param rig Camera rig whose camera is configured.
/// @param width Dead-zone width passed to Camera2D.
/// @param height Dead-zone height passed to Camera2D.
/// @details A disconnected rig does nothing.
void rt_camerarig2d_set_deadzone(void *rig, int64_t width, int64_t height);
/// @brief Accumulates a persistent screen-shake displacement.
/// @param rig Camera rig to update.
/// @param x X displacement added with saturation.
/// @param y Y displacement added with saturation.
/// @details The offset does not decay automatically; call @ref
///          rt_camerarig2d_clear_shake to remove it.
void rt_camerarig2d_add_shake(void *rig, int64_t x, int64_t y);
/// @brief Clears both accumulated shake components.
/// @param rig Camera rig to update.
void rt_camerarig2d_clear_shake(void *rig);
/// @brief Advances the camera one smooth-follow step toward the target.
/// @param rig Camera rig to update.
/// @details This updates the underlying Camera2D position; shake is added only
///          by the render-position getters.
void rt_camerarig2d_update(void *rig);
/// @brief Gets the camera X position plus shake.
/// @param rig Camera rig to inspect.
/// @return Saturating sum of camera and shake X, or `0` if disconnected or invalid.
int64_t rt_camerarig2d_get_render_x(void *rig);
/// @brief Gets the camera Y position plus shake.
/// @param rig Camera rig to inspect.
/// @return Saturating sum of camera and shake Y, or `0` if disconnected or invalid.
int64_t rt_camerarig2d_get_render_y(void *rig);

//===----------------------------------------------------------------------===//
// Asset importers — texture packing, Aseprite, and Tiled map loading.
//===----------------------------------------------------------------------===//

/// @brief Creates a named-region texture atlas backed by a Pixels image.
/// @param pixels Pixels image passed to the underlying TexAtlas constructor.
/// @return An owned TexturePackerAtlas handle, or `NULL` if construction fails.
/// @details A null image raises a runtime trap.
void *rt_texturepackeratlas_new(void *pixels);
/// @brief Borrows the underlying TexAtlas handle.
/// @param packer TexturePackerAtlas to inspect.
/// @return The borrowed atlas, or `NULL` for an invalid handle.
/// @warning The packer retains ownership; callers must not release the result.
void *rt_texturepackeratlas_get_atlas(void *packer);
/// @brief Registers a named atlas region.
/// @param packer TexturePackerAtlas to update.
/// @param name Region name forwarded to TexAtlas.
/// @param x Source rectangle X coordinate.
/// @param y Source rectangle Y coordinate.
/// @param width Source rectangle width.
/// @param height Source rectangle height.
void rt_texturepackeratlas_add(
    void *packer, rt_string name, int64_t x, int64_t y, int64_t width, int64_t height);
/// @brief Queries whether a named region exists.
/// @param packer TexturePackerAtlas to inspect.
/// @param name Region name to find.
/// @return Nonzero if the underlying atlas contains the name, otherwise `0`.
int64_t rt_texturepackeratlas_has(void *packer, rt_string name);
/// @brief Gets the number of registered regions.
/// @param packer TexturePackerAtlas to inspect.
/// @return The underlying atlas region count, or `0` for an invalid handle.
int64_t rt_texturepackeratlas_region_count(void *packer);

/// @brief Creates an importer with an unset frame grid.
/// @return An owned AsepriteImporter handle, or `NULL` if allocation fails.
void *rt_asepriteimporter_new(void);
/// @brief Sets the uniform frame dimensions used for atlas slicing.
/// @param importer Aseprite importer to update.
/// @param frame_width Positive frame width normalized to a valid dimension;
///        non-positive values leave that component unset as zero.
/// @param frame_height Positive frame height normalized to a valid dimension;
///        non-positive values leave that component unset as zero.
void rt_asepriteimporter_set_grid(void *importer, int64_t frame_width, int64_t frame_height);
/// @brief Gets the configured frame width.
/// @param importer Aseprite importer to inspect.
/// @return Width in pixels, or `0` when unset or invalid.
int64_t rt_asepriteimporter_get_frame_width(void *importer);
/// @brief Gets the configured frame height.
/// @param importer Aseprite importer to inspect.
/// @return Height in pixels, or `0` when unset or invalid.
int64_t rt_asepriteimporter_get_frame_height(void *importer);
/// @brief Slices a source image into a uniform-grid TexAtlas.
/// @param importer Aseprite importer supplying frame dimensions.
/// @param pixels Source Pixels image passed to the atlas loader.
/// @return A new owned TexAtlas handle, or `NULL` if the importer, image, grid,
///         or atlas construction is invalid.
void *rt_asepriteimporter_to_atlas(void *importer, void *pixels);

/// @brief Creates a Tiled map loader with a 16-by-16 default tile size.
/// @return An owned TiledMapLoader handle, or `NULL` if allocation fails.
void *rt_tiledmaploader_new(void);
/// @brief Sets the tile dimensions used for newly created and imported maps.
/// @param loader Tiled map loader to update.
/// @param tile_width Requested width normalized to the valid dimension range.
/// @param tile_height Requested height normalized to the valid dimension range.
void rt_tiledmaploader_set_tile_size(void *loader, int64_t tile_width, int64_t tile_height);
/// @brief Gets the configured tile width.
/// @param loader Tiled map loader to inspect.
/// @return Width in pixels, or `0` for an invalid handle.
int64_t rt_tiledmaploader_get_tile_width(void *loader);
/// @brief Gets the configured tile height.
/// @param loader Tiled map loader to inspect.
/// @return Height in pixels, or `0` for an invalid handle.
int64_t rt_tiledmaploader_get_tile_height(void *loader);
/// @brief Creates an empty Tilemap using the loader's tile dimensions.
/// @param loader Tiled map loader supplying tile width and height.
/// @param width Requested map width in cells.
/// @param height Requested map height in cells.
/// @return A new owned Tilemap handle, or `NULL` for an invalid loader or
///         Tilemap construction failure.
void *rt_tiledmaploader_new_tilemap(void *loader, int64_t width, int64_t height);
/// @brief Loads a filesystem Tiled JSON or TMX map as a render-ready Tilemap.
/// @param loader TiledMapLoader receiver supplying fallback tile settings.
/// @param path Filesystem root map path.
/// @return A new owned Tilemap, or `NULL` on receiver, import, or rendering failure.
void *rt_tiledmaploader_load(void *loader, rt_string path);
/// @brief Loads a filesystem Tiled map with explicit Result semantics.
/// @param loader TiledMapLoader receiver.
/// @param path Filesystem root map path.
/// @return An owned Result containing a Tilemap on success or an error string.
void *rt_tiledmaploader_load_result(void *loader, rt_string path);
/// @brief Loads an asset-backed Tiled map and dependencies as a Tilemap.
/// @param loader TiledMapLoader receiver supplying fallback tile settings.
/// @param path Logical asset root path.
/// @return A new owned Tilemap, or `NULL` on receiver, import, or rendering failure.
void *rt_tiledmaploader_load_asset(void *loader, rt_string path);
/// @brief Loads an asset-backed Tiled map with explicit Result semantics.
/// @param loader TiledMapLoader receiver.
/// @param path Logical asset root path.
/// @return An owned Result containing a Tilemap on success or an error string.
void *rt_tiledmaploader_load_asset_result(void *loader, rt_string path);

#ifdef __cplusplus
}
#endif
