//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics2d_extended.inc
/// @brief Implements the higher-level state, animation, layout, collision,
/// color, camera, graph, and import helpers for the 2D runtime.
///
/// @details This include fragment is consumed only by rt_graphics2d.c and
/// depends on the private types and helpers declared by earlier fragments.
/// It owns the implementations for Transform2D, sampler and blend state,
/// SpriteRenderer2D, tilemap rendering utilities, clip-driven sprites,
/// TextLayout2D, render passes and graphs, collision helpers, palettes and
/// gradients, CameraRig2D, and the lightweight atlas/importer adapters.
///
// File: src/runtime/graphics/2d/rt_graphics2d_extended.inc
// Purpose: Second-tier 2D utility classes including transforms, samplers, collision masks,
// palettes, camera rigs, importers, animation, text layout, and render graph helpers.
//
// Key invariants:
//   - Included only by src/runtime/graphics/2d/rt_graphics2d.c.
//   - Keeps the original rt_graphics2d.c declaration order and static helper
//     visibility; this is a physical source split, not a behavior split.
//   - Public runtime entry points preserve their existing names and signatures.
//
// Ownership/Lifetime:
//   - Ownership rules are unchanged from the original monolithic source file;
//     GC finalizers release retained handles and heap buffers for each class.
//
// Links: src/runtime/graphics/2d/rt_graphics2d.c,
//        src/runtime/graphics/2d/rt_graphics2d_internal.h
//
//===----------------------------------------------------------------------===//

//=============================================================================
// Additional 2D Game Graphics Helpers
//=============================================================================

typedef struct {
    int64_t x;
    int64_t y;
    int64_t scale_x;
    int64_t scale_y;
    int64_t rotation;
    int64_t origin_x;
    int64_t origin_y;
} rt_transform2d_impl;

typedef struct {
    int64_t filter;
    int64_t wrap;
} rt_sampler2d_impl;

typedef struct {
    int64_t blend_mode;
    int64_t tint;
    int64_t alpha;
} rt_blendstate2d_impl;

typedef struct {
    void *material;
    void *sampler;
    void *blend_state;
} rt_spriterenderer2d_impl;

typedef struct {
    int64_t chunk_width;
    int64_t chunk_height;
    int64_t dirty_count;
} rt_tilechunkcache2d_impl;

typedef struct {
    void *cache;
    int64_t draw_count;
} rt_tilemaprenderer2d_impl;

typedef struct {
    int64_t start_frame;
    int64_t frame_count;
    int64_t frame_delay_ms;
    int64_t loop;
} rt_animationclip2d_impl;

typedef struct {
    void *sprite;
    void *clip;
    int64_t elapsed_ms;
    int64_t frame;
    int64_t playing;
} rt_animatedsprite2d_impl;

typedef struct {
    void *font;
    int64_t scale;
    int64_t wrap_width;
    int64_t alignment;
    int64_t color;
} rt_textlayout2d_impl;

typedef struct {
    void *source;
    void *target;
    void *shader;
    int64_t enabled;
} rt_renderpass2d_impl;

typedef struct {
    void **passes;
    int64_t count;
    int64_t capacity;
} rt_rendergraph2d_impl;

typedef struct {
    int64_t width;
    int64_t height;
    uint8_t *bits;
} rt_collisionmask2d_impl;

typedef struct {
    int64_t x;
    int64_t y;
    int64_t width;
    int64_t height;
} rt_hitbox2d_impl;

typedef struct {
    uint32_t colors[256];
    uint8_t defined[256];
    int64_t count;
} rt_palette2d_impl;

typedef struct {
    uint32_t start;
    uint32_t end;
    int64_t steps;
} rt_gradient2d_impl;

typedef struct {
    void *camera;
    int64_t target_x;
    int64_t target_y;
    int64_t smoothing;
    int64_t shake_x;
    int64_t shake_y;
} rt_camerarig2d_impl;

typedef struct {
    void *atlas;
} rt_texturepackeratlas_impl;

typedef struct {
    int64_t frame_width;
    int64_t frame_height;
} rt_asepriteimporter_impl;

typedef struct {
    int64_t tile_width;
    int64_t tile_height;
} rt_tiledmaploader_impl;

//=============================================================================
// Second-tier classes: Transform2D / CollisionMask2D / HitBox2D / Palette2D /
// Gradient2D / CameraRig2D / Atlas importers / TileChunkCache2D /
// TilemapRenderer2D / AnimationClip2D / AnimatedSprite2D / TextLayout2D /
// RenderPass2D / RenderGraph2D
//=============================================================================
// These are lower-traffic utility classes layered on top of the primary 2D
// surface. Transform2D is a 2×3 affine builder (used by Renderer2D callers
// that want rotated / scaled draws); CollisionMask2D is a byte-per-cell grid;
// the atlas importers are data-format adapters for external tooling; the
// CameraRig2D wraps Camera with shake/target-follow. Scale values use the
// same 100 == 1.0x fixed-point convention as Viewport2D's 1000 == 1.0x,
// except Transform2D scales are per-axis integers with 100 meaning "identity."

/// @brief Round a double to the nearest int64, away from zero.
/// @param value Floating-point value to convert.
/// @return The nearest signed 64-bit integer, saturated at the limits; negative
///         infinity maps to `INT64_MIN`, while positive infinity and NaN map to
///         `INT64_MAX`.
static int64_t round_double_to_i64(double value) {
    if (!isfinite(value))
        return value < 0.0 ? INT64_MIN : INT64_MAX;
    if (value >= (double)INT64_MAX)
        return INT64_MAX;
    if (value <= (double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)(value >= 0.0 ? value + 0.5 : value - 0.5);
}

/// @brief Apply a Transform2D to a point: translate, pivot around origin, scale,
///        rotate, translate back.
/// @details The transformation order matters. Each input point is:
///          1. Shifted into origin-relative space (`(x - origin_x, y - origin_y)`).
///          2. Scaled per-axis (fixed-point: `100` == 1.0x).
///          3. Rotated by `rotation` degrees.
///          4. Shifted back by `origin_x + x` (position plus origin).
///          Rotation uses double-precision `cos/sin` since the `PI/180`
///          conversion already forces a float round-trip. Final coordinates
///          are rounded away from zero via `round_double_to_i64` so a point
///          that lands *exactly* between pixels snaps consistently
///          regardless of sign. Passing a NULL transform is a no-op —
///          input point is copied straight to output (identity).
/// @param impl Borrowed transform implementation, or `NULL` for identity.
/// @param x Input point X coordinate.
/// @param y Input point Y coordinate.
/// @param out_x Optional output for the transformed X coordinate.
/// @param out_y Optional output for the transformed Y coordinate.
static void transform2d_point(
    rt_transform2d_impl *impl, int64_t x, int64_t y, int64_t *out_x, int64_t *out_y) {
    if (!impl) {
        if (out_x)
            *out_x = x;
        if (out_y)
            *out_y = y;
        return;
    }

    double px = ((double)x - (double)impl->origin_x) * (double)impl->scale_x / 100.0;
    double py = ((double)y - (double)impl->origin_y) * (double)impl->scale_y / 100.0;
    double radians = (double)impl->rotation * RT2D_PI / 180.0;
    double c = cos(radians);
    double s = sin(radians);
    double rx = px * c - py * s;
    double ry = px * s + py * c;

    if (out_x)
        *out_x = rt2d_saturating_add_i64(rt2d_saturating_add_i64(impl->x, impl->origin_x),
                                    round_double_to_i64(rx));
    if (out_y)
        *out_y = rt2d_saturating_add_i64(rt2d_saturating_add_i64(impl->y, impl->origin_y),
                                    round_double_to_i64(ry));
}

/// @brief Allocate a Transform2D with identity state (pos=0, scale=100, rotation=0, origin=0).
/// @details Scale is initialized to 100, which equals 1.0× in the fixed-point convention
///          used throughout (100 == 1.0×, 200 == 2.0×, 50 == 0.5×).
/// @return A new owned Transform2D, or `NULL` on allocation failure.
void *rt_transform2d_new(void) {
    rt_transform2d_impl *impl = (rt_transform2d_impl *)rt_obj_new_i64(
        RT2D_TRANSFORM_CLASS_ID, (int64_t)sizeof(rt_transform2d_impl));
    if (!impl)
        return NULL;
    impl->scale_x = 100;
    impl->scale_y = 100;
    return impl;
}

/// @brief Return the X world-space position of the transform.
/// @param transform Borrowed Transform2D handle.
/// @return The X position, or `0` for an invalid handle.
int64_t rt_transform2d_get_x(void *transform) {
    return rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
               ? ((rt_transform2d_impl *)transform)->x
               : 0;
}

/// @brief Set the X world-space position of the transform.
/// @param transform Borrowed Transform2D handle to update.
/// @param x New X position.
void rt_transform2d_set_x(void *transform, int64_t x) {
    if (rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        ((rt_transform2d_impl *)transform)->x = x;
}

/// @brief Return the Y world-space position of the transform.
/// @param transform Borrowed Transform2D handle.
/// @return The Y position, or `0` for an invalid handle.
int64_t rt_transform2d_get_y(void *transform) {
    return rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
               ? ((rt_transform2d_impl *)transform)->y
               : 0;
}

/// @brief Set the Y world-space position of the transform.
/// @param transform Borrowed Transform2D handle to update.
/// @param y New Y position.
void rt_transform2d_set_y(void *transform, int64_t y) {
    if (rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        ((rt_transform2d_impl *)transform)->y = y;
}

/// @brief Return the X scale in fixed-point units (100 == 1.0×).
/// @param transform Borrowed Transform2D handle.
/// @return The X scale, or identity scale `100` for an invalid handle.
int64_t rt_transform2d_get_scale_x(void *transform) {
    return rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
               ? ((rt_transform2d_impl *)transform)->scale_x
               : 100;
}

/// @brief Set the X scale; clamped to [1, 10000] (0.01× to 100×).
/// @param transform Borrowed Transform2D handle to update.
/// @param scale_x Requested fixed-point X scale.
void rt_transform2d_set_scale_x(void *transform, int64_t scale_x) {
    if (rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        ((rt_transform2d_impl *)transform)->scale_x = clamp_i64(scale_x, 1, 10000);
}

/// @brief Return the Y scale in fixed-point units (100 == 1.0×).
/// @param transform Borrowed Transform2D handle.
/// @return The Y scale, or identity scale `100` for an invalid handle.
int64_t rt_transform2d_get_scale_y(void *transform) {
    return rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
               ? ((rt_transform2d_impl *)transform)->scale_y
               : 100;
}

/// @brief Set the Y scale; clamped to [1, 10000].
/// @param transform Borrowed Transform2D handle to update.
/// @param scale_y Requested fixed-point Y scale.
void rt_transform2d_set_scale_y(void *transform, int64_t scale_y) {
    if (rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        ((rt_transform2d_impl *)transform)->scale_y = clamp_i64(scale_y, 1, 10000);
}

/// @brief Return the rotation in integer degrees (no clamping — can be any multiple of 1°).
/// @param transform Borrowed Transform2D handle.
/// @return The stored rotation, or `0` for an invalid handle.
int64_t rt_transform2d_get_rotation(void *transform) {
    return rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
               ? ((rt_transform2d_impl *)transform)->rotation
               : 0;
}

/// @brief Set the rotation in integer degrees.
/// @param transform Borrowed Transform2D handle to update.
/// @param degrees New unnormalized rotation.
void rt_transform2d_set_rotation(void *transform, int64_t degrees) {
    if (rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        ((rt_transform2d_impl *)transform)->rotation = degrees;
}

/// @brief Set both X and Y position in one call.
/// @param transform Borrowed Transform2D handle to update.
/// @param x New X position.
/// @param y New Y position.
void rt_transform2d_set_position(void *transform, int64_t x, int64_t y) {
    if (!rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        return;
    rt_transform2d_impl *impl = (rt_transform2d_impl *)transform;
    impl->x = x;
    impl->y = y;
}

/// @brief Set X and Y scale in one call (each clamped to [1, 10000]).
/// @param transform Borrowed Transform2D handle to update.
/// @param scale_x Requested fixed-point X scale.
/// @param scale_y Requested fixed-point Y scale.
void rt_transform2d_set_scale(void *transform, int64_t scale_x, int64_t scale_y) {
    rt_transform2d_set_scale_x(transform, scale_x);
    rt_transform2d_set_scale_y(transform, scale_y);
}

/// @brief Set the pivot / rotation origin as an offset from the transform position.
/// @param transform Borrowed Transform2D handle to update.
/// @param x Origin X coordinate.
/// @param y Origin Y coordinate.
void rt_transform2d_set_origin(void *transform, int64_t x, int64_t y) {
    if (!rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        return;
    rt_transform2d_impl *impl = (rt_transform2d_impl *)transform;
    impl->origin_x = x;
    impl->origin_y = y;
}

/// @brief Translate (move) the transform by `(dx, dy)` using saturating addition.
/// @param transform Borrowed Transform2D handle to update.
/// @param dx X displacement.
/// @param dy Y displacement.
void rt_transform2d_translate(void *transform, int64_t dx, int64_t dy) {
    if (!rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID))
        return;
    rt_transform2d_impl *impl = (rt_transform2d_impl *)transform;
    impl->x = rt2d_saturating_add_i64(impl->x, dx);
    impl->y = rt2d_saturating_add_i64(impl->y, dy);
}

/// @brief Return the transformed X coordinate for input point `(x, y)`.
/// @details Delegates to `transform2d_point`; useful when callers only need the X component.
/// @param transform Borrowed Transform2D handle, or invalid for identity.
/// @param x Input point X coordinate.
/// @param y Input point Y coordinate.
/// @return The transformed X coordinate, or @p x when the handle is invalid.
int64_t rt_transform2d_transform_x(void *transform, int64_t x, int64_t y) {
    int64_t out_x = x;
    transform2d_point(rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
                          ? (rt_transform2d_impl *)transform
                          : NULL,
                      x,
                      y,
                      &out_x,
                      NULL);
    return out_x;
}

/// @brief Return the transformed Y coordinate for input point `(x, y)`.
/// @details Delegates to `transform2d_point`; useful when callers only need the Y component.
/// @param transform Borrowed Transform2D handle, or invalid for identity.
/// @param x Input point X coordinate.
/// @param y Input point Y coordinate.
/// @return The transformed Y coordinate, or @p y when the handle is invalid.
int64_t rt_transform2d_transform_y(void *transform, int64_t x, int64_t y) {
    int64_t out_y = y;
    transform2d_point(rt2d_has_class(transform, RT2D_TRANSFORM_CLASS_ID)
                          ? (rt_transform2d_impl *)transform
                          : NULL,
                      x,
                      y,
                      NULL,
                      &out_y);
    return out_y;
}

/// @brief Allocate a Sampler2D with default state (nearest filter, clamp wrap).
/// @return A new owned Sampler2D, or `NULL` on allocation failure.
void *rt_sampler2d_new(void) {
    rt_sampler2d_impl *impl = (rt_sampler2d_impl *)rt_obj_new_i64(
        RT2D_SAMPLER_CLASS_ID, (int64_t)sizeof(rt_sampler2d_impl));
    if (!impl)
        return NULL;
    impl->filter = RT_GRAPHICS2D_FILTER_NEAREST;
    impl->wrap = RT_GRAPHICS2D_WRAP_CLAMP;
    return impl;
}

/// @brief Set the filter mode; unrecognised values fall back to NEAREST.
/// @param sampler Borrowed Sampler2D handle to update.
/// @param filter Requested filter mode.
void rt_sampler2d_set_filter(void *sampler, int64_t filter) {
    if (rt2d_has_class(sampler, RT2D_SAMPLER_CLASS_ID))
        ((rt_sampler2d_impl *)sampler)->filter = filter == RT_GRAPHICS2D_FILTER_LINEAR
                                                     ? RT_GRAPHICS2D_FILTER_LINEAR
                                                     : RT_GRAPHICS2D_FILTER_NEAREST;
}

/// @brief Return the current filter mode (NEAREST or LINEAR).
/// @param sampler Borrowed Sampler2D handle.
/// @return The stored filter, or nearest filtering for an invalid handle.
int64_t rt_sampler2d_get_filter(void *sampler) {
    return rt2d_has_class(sampler, RT2D_SAMPLER_CLASS_ID) ? ((rt_sampler2d_impl *)sampler)->filter
                                                          : RT_GRAPHICS2D_FILTER_NEAREST;
}

/// @brief Set the wrap mode; unrecognised values fall back to CLAMP.
/// @param sampler Borrowed Sampler2D handle to update.
/// @param wrap Requested wrap mode.
void rt_sampler2d_set_wrap(void *sampler, int64_t wrap) {
    if (rt2d_has_class(sampler, RT2D_SAMPLER_CLASS_ID))
        ((rt_sampler2d_impl *)sampler)->wrap = wrap == RT_GRAPHICS2D_WRAP_REPEAT
                                                   ? RT_GRAPHICS2D_WRAP_REPEAT
                                                   : RT_GRAPHICS2D_WRAP_CLAMP;
}

/// @brief Return the current wrap mode (CLAMP or REPEAT).
/// @param sampler Borrowed Sampler2D handle.
/// @return The stored wrap mode, or clamp wrapping for an invalid handle.
int64_t rt_sampler2d_get_wrap(void *sampler) {
    return rt2d_has_class(sampler, RT2D_SAMPLER_CLASS_ID) ? ((rt_sampler2d_impl *)sampler)->wrap
                                                          : RT_GRAPHICS2D_WRAP_CLAMP;
}

/// @brief Push the sampler's filter and wrap settings into a Texture2D.
/// @details Convenience combinator — equivalent to calling `SetFilter` + `SetWrap`
///          on the texture directly, but expressed through the sampler's parameters.
/// @param sampler Borrowed source Sampler2D.
/// @param texture Borrowed destination Texture2D.
void rt_sampler2d_apply_to_texture(void *sampler, void *texture) {
    if (!rt2d_has_class(sampler, RT2D_SAMPLER_CLASS_ID) || !texture)
        return;
    rt_texture2d_set_filter(texture, rt_sampler2d_get_filter(sampler));
    rt_texture2d_set_wrap(texture, rt_sampler2d_get_wrap(sampler));
}

/// @brief Allocate a BlendState2D with default state (alpha blend, no tint, full opacity).
/// @details `tint` defaults to -1 meaning "no tint override"; `alpha` defaults to 255.
/// @return A new owned BlendState2D, or `NULL` on allocation failure.
void *rt_blendstate2d_new(void) {
    rt_blendstate2d_impl *impl = (rt_blendstate2d_impl *)rt_obj_new_i64(
        RT2D_BLENDSTATE_CLASS_ID, (int64_t)sizeof(rt_blendstate2d_impl));
    if (!impl)
        return NULL;
    impl->blend_mode = RT_GRAPHICS2D_BLEND_ALPHA;
    impl->tint = -1;
    impl->alpha = 255;
    return impl;
}

/// @brief Set the blend mode; clamped to [0, 2] (ALPHA / OPAQUE / ADDITIVE).
/// @param state Borrowed BlendState2D handle to update.
/// @param blend_mode Requested blend mode.
void rt_blendstate2d_set_blend_mode(void *state, int64_t blend_mode) {
    if (rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID))
        ((rt_blendstate2d_impl *)state)->blend_mode = clamp_i64(blend_mode, 0, 2);
}

/// @brief Return the current blend mode.
/// @param state Borrowed BlendState2D handle.
/// @return The stored mode, or alpha blending for an invalid handle.
int64_t rt_blendstate2d_get_blend_mode(void *state) {
    return rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID)
               ? ((rt_blendstate2d_impl *)state)->blend_mode
               : RT_GRAPHICS2D_BLEND_ALPHA;
}

/// @brief Set the tint as 0x00RRGGBB; pass a negative value to disable tint.
/// @param state Borrowed BlendState2D handle to update.
/// @param rgb Requested tint or a negative disable sentinel.
void rt_blendstate2d_set_tint(void *state, int64_t rgb) {
    if (rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID))
        ((rt_blendstate2d_impl *)state)->tint = rgb < 0 ? -1 : (rgb & 0x00FFFFFF);
}

/// @brief Return the tint color (0x00RRGGBB), or -1 if no tint is set.
/// @param state Borrowed BlendState2D handle.
/// @return The stored tint, or `-1` when disabled or invalid.
int64_t rt_blendstate2d_get_tint(void *state) {
    return rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID) ? ((rt_blendstate2d_impl *)state)->tint
                                                           : -1;
}

/// @brief Set the global alpha in [0, 255]; values are clamped.
/// @param state Borrowed BlendState2D handle to update.
/// @param alpha Requested alpha scale.
void rt_blendstate2d_set_alpha(void *state, int64_t alpha) {
    if (rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID))
        ((rt_blendstate2d_impl *)state)->alpha = clamp_u8_i64(alpha);
}

/// @brief Return the current global alpha in [0, 255].
/// @param state Borrowed BlendState2D handle.
/// @return The stored alpha, or `255` for an invalid handle.
int64_t rt_blendstate2d_get_alpha(void *state) {
    return rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID) ? ((rt_blendstate2d_impl *)state)->alpha
                                                           : 255;
}

/// @brief Push blend mode, alpha, and tint from this state into a Renderer2D.
/// @details Equivalent to three separate `SetBlendMode` / `SetAlpha` / `SetTint`
///          calls, combined into a single apply step.
/// @param state Borrowed source BlendState2D.
/// @param renderer Borrowed destination Renderer2D.
void rt_blendstate2d_apply_to_renderer(void *state, void *renderer) {
    if (!rt2d_has_class(state, RT2D_BLENDSTATE_CLASS_ID) || !renderer)
        return;
    rt_renderer2d_set_blend_mode(renderer, rt_blendstate2d_get_blend_mode(state));
    rt_renderer2d_set_alpha(renderer, rt_blendstate2d_get_alpha(state));
    rt_renderer2d_set_tint(renderer, rt_blendstate2d_get_tint(state));
}

/// @brief GC finalizer — releases retained references to material, sampler, and blend state.
/// @param obj SpriteRenderer2D payload being finalized.
static void spriterenderer2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_SPRITERENDERER_CLASS_ID))
        return;
    rt_spriterenderer2d_impl *impl = (rt_spriterenderer2d_impl *)obj;
    rt2d_release_ref_slot(&impl->material);
    rt2d_release_ref_slot(&impl->sampler);
    rt2d_release_ref_slot(&impl->blend_state);
}

/// @brief Allocate a SpriteRenderer2D with no material, sampler, or blend state bound.
/// @return A new owned SpriteRenderer2D, or `NULL` on allocation failure.
void *rt_spriterenderer2d_new(void) {
    rt_spriterenderer2d_impl *impl = (rt_spriterenderer2d_impl *)rt_obj_new_i64(
        RT2D_SPRITERENDERER_CLASS_ID, (int64_t)sizeof(rt_spriterenderer2d_impl));
    if (!impl)
        return NULL;
    rt_obj_set_finalizer(impl, spriterenderer2d_finalize);
    return impl;
}

/// @brief Retain-before-release helper for all three sprite-renderer reference slots.
/// @param slot In-out retained-reference slot to replace.
/// @param value New borrowed value to retain, or `NULL`.
static void spriterenderer2d_set_ref(void **slot, void *value) {
    rt2d_retain_ref(value);
    rt2d_release_ref_slot(slot);
    *slot = value;
}

/// @brief Bind a Material2D to this renderer, retaining a reference.
/// @details When set, the material's tint, alpha, and blend-mode override the
///          renderer state each time `DrawPixels` or `DrawTexture` is called.
/// @param sprite_renderer Borrowed SpriteRenderer2D handle to update.
/// @param material Borrowed Material2D to retain, or `NULL` to clear.
void rt_spriterenderer2d_set_material(void *sprite_renderer, void *material) {
    if (rt2d_has_class(sprite_renderer, RT2D_SPRITERENDERER_CLASS_ID) &&
        (!material || rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID)))
        spriterenderer2d_set_ref(&((rt_spriterenderer2d_impl *)sprite_renderer)->material,
                                 material);
}

/// @brief Bind a Sampler2D to this renderer, retaining a reference.
/// @details When set, the sampler's filter and wrap settings override the texture's
///          own settings during `DrawTexture`.
/// @param sprite_renderer Borrowed SpriteRenderer2D handle to update.
/// @param sampler Borrowed Sampler2D to retain, or `NULL` to clear.
void rt_spriterenderer2d_set_sampler(void *sprite_renderer, void *sampler) {
    if (rt2d_has_class(sprite_renderer, RT2D_SPRITERENDERER_CLASS_ID) &&
        (!sampler || rt2d_has_class(sampler, RT2D_SAMPLER_CLASS_ID)))
        spriterenderer2d_set_ref(&((rt_spriterenderer2d_impl *)sprite_renderer)->sampler, sampler);
}

/// @brief Bind a BlendState2D to this renderer, retaining a reference.
/// @details BlendState overrides are applied *after* material overrides, so the
///          blend state wins if both are bound and specify the same property.
/// @param sprite_renderer Borrowed SpriteRenderer2D handle to update.
/// @param blend_state Borrowed BlendState2D to retain, or `NULL` to clear.
void rt_spriterenderer2d_set_blend_state(void *sprite_renderer, void *blend_state) {
    if (rt2d_has_class(sprite_renderer, RT2D_SPRITERENDERER_CLASS_ID) &&
        (!blend_state || rt2d_has_class(blend_state, RT2D_BLENDSTATE_CLASS_ID)))
        spriterenderer2d_set_ref(&((rt_spriterenderer2d_impl *)sprite_renderer)->blend_state,
                                 blend_state);
}

/// @brief Push material and blend-state overrides from the sprite renderer onto the renderer.
/// @details When a material is assigned, its tint, alpha, and blend mode are forwarded to
///          @p renderer. An explicit blend state then layers on top via
///          `rt_blendstate2d_apply_to_renderer`. Called before every draw operation so the
///          renderer reflects the sprite renderer's current configuration.
/// @param impl Borrowed sprite-renderer implementation supplying overrides.
/// @param renderer Borrowed Renderer2D receiving temporary state.
static void spriterenderer2d_apply_state(rt_spriterenderer2d_impl *impl, void *renderer) {
    if (!impl || !renderer)
        return;
    if (impl->material) {
        rt_renderer2d_set_tint(renderer, rt_material2d_get_tint(impl->material));
        rt_renderer2d_set_alpha(renderer, rt_material2d_get_alpha(impl->material));
        rt_renderer2d_set_blend_mode(renderer, rt_material2d_get_blend_mode(impl->material));
    }
    if (impl->blend_state)
        rt_blendstate2d_apply_to_renderer(impl->blend_state, renderer);
}

typedef struct {
    int64_t tint;
    int64_t alpha;
    int64_t blend_mode;
} rt_renderer2d_saved_state;

/// @brief Snapshot a renderer's tint/alpha/blend mode into @p state so it can
///        be restored after a temporary override (see renderer2d_restore_state).
/// @param renderer Borrowed renderer implementation to snapshot.
/// @param state Output state structure.
static void renderer2d_save_state(rt_renderer2d_impl *renderer, rt_renderer2d_saved_state *state) {
    if (!renderer || !state)
        return;
    state->tint = renderer->tint;
    state->alpha = renderer->alpha;
    state->blend_mode = renderer->blend_mode;
}

/// @brief Restore a renderer's tint/alpha/blend mode previously captured by
///        renderer2d_save_state.
/// @param renderer Borrowed renderer implementation to update.
/// @param state Borrowed saved state to restore.
static void renderer2d_restore_state(rt_renderer2d_impl *renderer,
                                     const rt_renderer2d_saved_state *state) {
    if (!renderer || !state)
        return;
    renderer->tint = state->tint;
    renderer->alpha = state->alpha;
    renderer->blend_mode = state->blend_mode;
}

/// @brief Apply state and draw a Pixels buffer into the Renderer2D at `(x, y)`.
/// @details Pushes material/blend-state overrides onto the renderer, then issues
///          `rt_renderer2d_draw_pixels`. The renderer's prior tint/alpha/blend-mode
///          are restored after the draw is queued.
/// @param sprite_renderer Borrowed SpriteRenderer2D supplying pipeline state.
/// @param renderer Borrowed active Renderer2D receiving the command.
/// @param pixels Borrowed source Pixels handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
void rt_spriterenderer2d_draw_pixels(
    void *sprite_renderer, void *renderer, void *pixels, int64_t x, int64_t y) {
    rt_spriterenderer2d_impl *impl = rt2d_has_class(sprite_renderer, RT2D_SPRITERENDERER_CLASS_ID)
                                         ? (rt_spriterenderer2d_impl *)sprite_renderer
                                         : NULL;
    rt_renderer2d_impl *renderer_impl = renderer2d_checked(renderer);
    if (!impl || !renderer_impl)
        return;
    rt_renderer2d_saved_state state;
    renderer2d_save_state(renderer_impl, &state);
    spriterenderer2d_apply_state(impl, renderer);
    rt_renderer2d_draw_pixels(renderer, pixels, x, y);
    renderer2d_restore_state(renderer_impl, &state);
}

/// @brief Apply state and draw a Texture2D into the Renderer2D at `(x, y)`.
/// @details Same state-push as `DrawPixels`, but also resolves filter/wrap from
///          the bound sampler (if any) before queuing the texture draw command.
/// @param sprite_renderer Borrowed SpriteRenderer2D supplying pipeline state.
/// @param renderer Borrowed active Renderer2D receiving the command.
/// @param texture Borrowed source Texture2D handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
void rt_spriterenderer2d_draw_texture(
    void *sprite_renderer, void *renderer, void *texture, int64_t x, int64_t y) {
    rt_spriterenderer2d_impl *impl = rt2d_has_class(sprite_renderer, RT2D_SPRITERENDERER_CLASS_ID)
                                         ? (rt_spriterenderer2d_impl *)sprite_renderer
                                         : NULL;
    rt_renderer2d_impl *renderer_impl = renderer2d_checked(renderer);
    if (!impl || !renderer_impl || !texture2d_checked(texture))
        return;
    rt_renderer2d_saved_state state;
    renderer2d_save_state(renderer_impl, &state);
    spriterenderer2d_apply_state(impl, renderer);
    int64_t filter =
        impl->sampler ? rt_sampler2d_get_filter(impl->sampler) : rt_texture2d_get_filter(texture);
    int64_t wrap =
        impl->sampler ? rt_sampler2d_get_wrap(impl->sampler) : rt_texture2d_get_wrap(texture);
    renderer2d_queue(renderer_impl,
                     1,
                     texture,
                     x,
                     y,
                     0,
                     0,
                     rt_texture2d_width(texture),
                     rt_texture2d_height(texture),
                     rt_texture2d_width(texture),
                     rt_texture2d_height(texture),
                     filter,
                     wrap);
    renderer2d_restore_state(renderer_impl, &state);
}

/// @brief Allocate a TileChunkCache2D describing chunk dimensions for dirty-region tracking.
/// @details Chunk dimensions are sanitized via `normalized_dim`. Game code that modifies
///          tiles in a chunk calls `MarkDirty`; a renderer that caches per-chunk textures
///          checks `GetDirtyCount` and rebuilds stale chunks before drawing.
/// @param chunk_width Requested chunk width in tiles.
/// @param chunk_height Requested chunk height in tiles.
/// @return A new owned TileChunkCache2D, or `NULL` on allocation failure.
void *rt_tilechunkcache2d_new(int64_t chunk_width, int64_t chunk_height) {
    rt_tilechunkcache2d_impl *impl = (rt_tilechunkcache2d_impl *)rt_obj_new_i64(
        RT2D_TILECHUNKCACHE_CLASS_ID, (int64_t)sizeof(rt_tilechunkcache2d_impl));
    if (!impl)
        return NULL;
    impl->chunk_width = normalized_dim(chunk_width);
    impl->chunk_height = normalized_dim(chunk_height);
    return impl;
}

/// @brief Return the chunk width in tiles.
/// @param cache Borrowed TileChunkCache2D handle.
/// @return The normalized chunk width, or `0` for an invalid handle.
int64_t rt_tilechunkcache2d_get_chunk_width(void *cache) {
    return rt2d_has_class(cache, RT2D_TILECHUNKCACHE_CLASS_ID)
               ? ((rt_tilechunkcache2d_impl *)cache)->chunk_width
               : 0;
}

/// @brief Return the chunk height in tiles.
/// @param cache Borrowed TileChunkCache2D handle.
/// @return The normalized chunk height, or `0` for an invalid handle.
int64_t rt_tilechunkcache2d_get_chunk_height(void *cache) {
    return rt2d_has_class(cache, RT2D_TILECHUNKCACHE_CLASS_ID)
               ? ((rt_tilechunkcache2d_impl *)cache)->chunk_height
               : 0;
}

/// @brief Increment the dirty counter — signals that at least one chunk needs rebuild.
/// @param cache Borrowed TileChunkCache2D handle to update.
void rt_tilechunkcache2d_mark_dirty(void *cache) {
    if (!rt2d_has_class(cache, RT2D_TILECHUNKCACHE_CLASS_ID))
        return;
    rt_tilechunkcache2d_impl *impl = (rt_tilechunkcache2d_impl *)cache;
    if (impl->dirty_count < INT64_MAX)
        impl->dirty_count++;
}

/// @brief Reset the dirty counter to zero after the renderer has processed all dirty chunks.
/// @param cache Borrowed TileChunkCache2D handle to update.
void rt_tilechunkcache2d_clear_dirty(void *cache) {
    if (rt2d_has_class(cache, RT2D_TILECHUNKCACHE_CLASS_ID))
        ((rt_tilechunkcache2d_impl *)cache)->dirty_count = 0;
}

/// @brief Return the number of pending dirty-chunk notifications.
/// @param cache Borrowed TileChunkCache2D handle.
/// @return The pending notification count, or `0` for an invalid handle.
int64_t rt_tilechunkcache2d_get_dirty_count(void *cache) {
    return rt2d_has_class(cache, RT2D_TILECHUNKCACHE_CLASS_ID)
               ? ((rt_tilechunkcache2d_impl *)cache)->dirty_count
               : 0;
}

/// @brief GC finalizer — releases the retained TileChunkCache2D reference.
/// @param obj TileMapRenderer2D payload being finalized.
static void tilemaprenderer2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_TILEMAPRENDERER_CLASS_ID))
        return;
    rt_tilemaprenderer2d_impl *impl = (rt_tilemaprenderer2d_impl *)obj;
    rt2d_release_ref_slot(&impl->cache);
}

/// @brief Allocate a TilemapRenderer2D with no chunk cache bound and zero draw count.
/// @return A new owned TileMapRenderer2D, or `NULL` on allocation failure.
void *rt_tilemaprenderer2d_new(void) {
    rt_tilemaprenderer2d_impl *impl = (rt_tilemaprenderer2d_impl *)rt_obj_new_i64(
        RT2D_TILEMAPRENDERER_CLASS_ID, (int64_t)sizeof(rt_tilemaprenderer2d_impl));
    if (!impl)
        return NULL;
    rt_obj_set_finalizer(impl, tilemaprenderer2d_finalize);
    return impl;
}

/// @brief Bind a TileChunkCache2D to this renderer, retaining a reference.
/// @param renderer Borrowed TileMapRenderer2D handle to update.
/// @param cache Borrowed TileChunkCache2D to retain, or `NULL` to clear.
void rt_tilemaprenderer2d_set_chunk_cache(void *renderer, void *cache) {
    if (rt2d_has_class(renderer, RT2D_TILEMAPRENDERER_CLASS_ID) &&
        (!cache || rt2d_has_class(cache, RT2D_TILECHUNKCACHE_CLASS_ID)))
        spriterenderer2d_set_ref(&((rt_tilemaprenderer2d_impl *)renderer)->cache, cache);
}

/// @brief Return the number of tiles drawn by the last Draw/DrawRegion call.
/// @param renderer Borrowed TileMapRenderer2D handle.
/// @return The last recorded visible-tile count, or `0` for an invalid handle.
int64_t rt_tilemaprenderer2d_get_draw_count(void *renderer) {
    return rt2d_has_class(renderer, RT2D_TILEMAPRENDERER_CLASS_ID)
               ? ((rt_tilemaprenderer2d_impl *)renderer)->draw_count
               : 0;
}

/// @brief Draw the entire tilemap into `canvas` offset by `(offset_x, offset_y)`.
/// @details Delegates to `rt_tilemap_draw` and records the number of visible
///          non-empty tiles issued by that draw.
/// @param renderer Borrowed TileMapRenderer2D recording the count.
/// @param tilemap Borrowed Tilemap to render.
/// @param canvas Borrowed destination Canvas.
/// @param offset_x Destination X offset.
/// @param offset_y Destination Y offset.
void rt_tilemaprenderer2d_draw(
    void *renderer, void *tilemap, void *canvas, int64_t offset_x, int64_t offset_y) {
    if (!rt2d_has_class(renderer, RT2D_TILEMAPRENDERER_CLASS_ID) || !tilemap || !canvas)
        return;
    rt_tilemaprenderer2d_impl *impl = (rt_tilemaprenderer2d_impl *)renderer;
    impl->draw_count = rt_tilemap_count_drawn_visible(tilemap, canvas, offset_x, offset_y);
    rt_tilemap_draw(tilemap, canvas, offset_x, offset_y);
}

/// @brief Draw a viewport-clipped region of the tilemap into `canvas`.
/// @details Delegates to `rt_tilemap_draw_region`, culling tiles outside the
///          `(view_x, view_y, view_w, view_h)` rectangle before drawing and
///          records how many non-empty tiles were visible.
/// @param renderer Borrowed TileMapRenderer2D recording the count.
/// @param tilemap Borrowed Tilemap to render.
/// @param canvas Borrowed destination Canvas.
/// @param offset_x Destination X offset.
/// @param offset_y Destination Y offset.
/// @param view_x View rectangle X coordinate.
/// @param view_y View rectangle Y coordinate.
/// @param view_w View rectangle width.
/// @param view_h View rectangle height.
void rt_tilemaprenderer2d_draw_region(void *renderer,
                                      void *tilemap,
                                      void *canvas,
                                      int64_t offset_x,
                                      int64_t offset_y,
                                      int64_t view_x,
                                      int64_t view_y,
                                      int64_t view_w,
                                      int64_t view_h) {
    if (!rt2d_has_class(renderer, RT2D_TILEMAPRENDERER_CLASS_ID) || !tilemap || !canvas)
        return;
    rt_tilemaprenderer2d_impl *impl = (rt_tilemaprenderer2d_impl *)renderer;
    impl->draw_count = rt_tilemap_count_drawn_region(tilemap, view_x, view_y, view_w, view_h);
    rt_tilemap_draw_region(tilemap, canvas, offset_x, offset_y, view_x, view_y, view_w, view_h);
}

/// @brief Allocate an AnimationClip2D — a frame-range description (starting
///        sprite frame, frame count, per-frame delay in ms, looping flag).
/// @details Defensive defaults for malformed inputs: negative `start_frame` → 0,
///          non-positive `frame_count` → 1 (single-frame clip), non-positive
///          `frame_delay_ms` → 100ms (~10 fps default). Clip data is immutable
///          after construction; callers that need different parameters build
///          a new clip rather than mutating.
/// @param start_frame Requested first sprite frame.
/// @param frame_count Requested number of frames.
/// @param frame_delay_ms Requested per-frame duration in milliseconds.
/// @param loop Nonzero to loop.
/// @return A new owned AnimationClip2D, or `NULL` on allocation failure.
void *rt_animationclip2d_new(int64_t start_frame,
                             int64_t frame_count,
                             int64_t frame_delay_ms,
                             int64_t loop) {
    rt_animationclip2d_impl *impl = (rt_animationclip2d_impl *)rt_obj_new_i64(
        RT2D_ANIMATIONCLIP_CLASS_ID, (int64_t)sizeof(rt_animationclip2d_impl));
    if (!impl)
        return NULL;
    impl->start_frame = start_frame < 0 ? 0 : start_frame;
    impl->frame_count = frame_count <= 0 ? 1 : frame_count;
    impl->frame_delay_ms = frame_delay_ms <= 0 ? 100 : frame_delay_ms;
    impl->loop = loop != 0;
    return impl;
}

/// @brief Return the first sprite frame index this clip plays from.
/// @param clip Borrowed AnimationClip2D handle.
/// @return The normalized start frame, or `0` for an invalid handle.
int64_t rt_animationclip2d_get_start_frame(void *clip) {
    return rt2d_has_class(clip, RT2D_ANIMATIONCLIP_CLASS_ID)
               ? ((rt_animationclip2d_impl *)clip)->start_frame
               : 0;
}

/// @brief Return the number of sprite frames in this clip.
/// @param clip Borrowed AnimationClip2D handle.
/// @return The positive frame count, or `0` for an invalid handle.
int64_t rt_animationclip2d_get_frame_count(void *clip) {
    return rt2d_has_class(clip, RT2D_ANIMATIONCLIP_CLASS_ID)
               ? ((rt_animationclip2d_impl *)clip)->frame_count
               : 0;
}

/// @brief Return the per-frame delay in milliseconds.
/// @param clip Borrowed AnimationClip2D handle.
/// @return The positive frame delay, or `0` for an invalid handle.
int64_t rt_animationclip2d_get_frame_delay_ms(void *clip) {
    return rt2d_has_class(clip, RT2D_ANIMATIONCLIP_CLASS_ID)
               ? ((rt_animationclip2d_impl *)clip)->frame_delay_ms
               : 0;
}

/// @brief Return non-zero if the clip loops back to its first frame after the last.
/// @param clip Borrowed AnimationClip2D handle.
/// @return `1` for a looping clip, otherwise `0`.
int64_t rt_animationclip2d_get_loop(void *clip) {
    return rt2d_has_class(clip, RT2D_ANIMATIONCLIP_CLASS_ID)
               ? ((rt_animationclip2d_impl *)clip)->loop
               : 0;
}

/// @brief GC finalizer — releases retained references to the sprite and animation clip.
/// @param obj AnimatedSprite2D payload being finalized.
static void animatedsprite2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_ANIMATEDSPRITE_CLASS_ID))
        return;
    rt_animatedsprite2d_impl *impl = (rt_animatedsprite2d_impl *)obj;
    rt2d_release_ref_slot(&impl->sprite);
    rt2d_release_ref_slot(&impl->clip);
}

/// @brief Allocate an AnimatedSprite2D wrapping `sprite`, ready to play clips.
/// @details Retains a reference to `sprite`. Playback remains stopped until a
///          valid clip is attached or `Play` is called with a valid clip.
/// @param sprite Borrowed valid Sprite to retain.
/// @return A new owned AnimatedSprite2D, or `NULL` for an invalid sprite or
///         allocation failure.
void *rt_animatedsprite2d_new(void *sprite) {
    if (!sprite || !rt_obj_is_instance(sprite, RT_SPRITE_CLASS_ID, 0))
        return NULL;
    rt2d_retain_ref(sprite);
    rt_animatedsprite2d_impl *impl = (rt_animatedsprite2d_impl *)rt_obj_new_i64(
        RT2D_ANIMATEDSPRITE_CLASS_ID, (int64_t)sizeof(rt_animatedsprite2d_impl));
    if (!impl) {
        void *owned_sprite = sprite;
        rt2d_release_ref_slot(&owned_sprite);
        return NULL;
    }
    impl->sprite = sprite;
    impl->clip = NULL;
    impl->elapsed_ms = 0;
    impl->frame = 0;
    impl->playing = 0;
    rt_obj_set_finalizer(impl, animatedsprite2d_finalize);
    return impl;
}

/// @brief Bind an AnimationClip2D and reset elapsed time and frame counter.
/// @details Calling `SetClip` with a non-NULL clip immediately seeks the bound
///          sprite to `clip->start_frame`, so the first drawn frame is always
///          consistent even before the first `Update`. Passing NULL stops playback.
/// @param animated_sprite Borrowed AnimatedSprite2D handle to update.
/// @param clip Borrowed AnimationClip2D to retain, or `NULL` to clear.
void rt_animatedsprite2d_set_clip(void *animated_sprite, void *clip) {
    if (!rt2d_has_class(animated_sprite, RT2D_ANIMATEDSPRITE_CLASS_ID) ||
        (clip && !rt2d_has_class(clip, RT2D_ANIMATIONCLIP_CLASS_ID)))
        return;
    rt_animatedsprite2d_impl *impl = (rt_animatedsprite2d_impl *)animated_sprite;
    spriterenderer2d_set_ref(&impl->clip, clip);
    impl->elapsed_ms = 0;
    impl->frame = 0;
    impl->playing = clip != NULL;
    if (impl->sprite && clip)
        rt_sprite_set_frame(impl->sprite, rt_animationclip2d_get_start_frame(clip));
}

/// @brief Resume playback after `Stop` (or after a non-looping clip finished).
/// @param animated_sprite Borrowed AnimatedSprite2D handle to play.
void rt_animatedsprite2d_play(void *animated_sprite) {
    rt_animatedsprite2d_impl *impl = rt2d_has_class(animated_sprite, RT2D_ANIMATEDSPRITE_CLASS_ID)
                                         ? (rt_animatedsprite2d_impl *)animated_sprite
                                         : NULL;
    if (!impl)
        return;
    if (!impl->sprite || !rt2d_has_class(impl->clip, RT2D_ANIMATIONCLIP_CLASS_ID)) {
        impl->playing = 0;
        return;
    }

    rt_animationclip2d_impl *clip = (rt_animationclip2d_impl *)impl->clip;
    int64_t sprite_frames = rt_sprite_get_frame_count(impl->sprite);
    int64_t available = sprite_frames > clip->start_frame ? sprite_frames - clip->start_frame : 0;
    int64_t effective_count = clip->frame_count < available ? clip->frame_count : available;
    if (effective_count <= 0) {
        impl->playing = 0;
        impl->frame = 0;
        impl->elapsed_ms = 0;
        return;
    }

    if (!impl->playing && impl->frame >= effective_count - 1) {
        impl->frame = 0;
        impl->elapsed_ms = 0;
        rt_sprite_set_frame(impl->sprite, clip->start_frame);
    }
    impl->playing = 1;
}

/// @brief Stop playback and reset to the clip's first frame.
/// @param animated_sprite Borrowed AnimatedSprite2D handle to stop.
void rt_animatedsprite2d_stop(void *animated_sprite) {
    if (!rt2d_has_class(animated_sprite, RT2D_ANIMATEDSPRITE_CLASS_ID))
        return;
    rt_animatedsprite2d_impl *impl = (rt_animatedsprite2d_impl *)animated_sprite;
    impl->playing = 0;
    impl->elapsed_ms = 0;
    impl->frame = 0;
    if (impl->sprite && rt2d_has_class(impl->clip, RT2D_ANIMATIONCLIP_CLASS_ID))
        rt_sprite_set_frame(impl->sprite, ((rt_animationclip2d_impl *)impl->clip)->start_frame);
}

/// @brief Advance an AnimatedSprite2D by `delta_ms`, stepping through clip frames
///        and optionally looping at the end.
/// @details Accumulates `delta_ms` into `elapsed_ms`, divides by the frame delay
///          to compute every elapsed frame step at once, and keeps the remainder.
///          A long update can therefore skip multiple frames without a
///          per-frame loop. Negative `delta_ms` is clamped to 0 (no rewind).
///
///          End-of-clip behavior:
///          - **Looping clip:** advances modulo the effective frame count and
///            preserves the sub-frame remainder.
///          - **Non-looping clip:** snaps to the last frame, clears `playing`,
///            and clears elapsed time when the computed steps reach the end.
///            Replaying requires a fresh `SetClip` or `Play`.
///
///          The sprite's own frame index is updated via `rt_sprite_set_frame`
///          as we step, so consumers that just `Draw` the sprite see the
///          correct frame without having to query the AnimatedSprite2D.
/// @param animated_sprite Borrowed AnimatedSprite2D handle to advance.
/// @param delta_ms Elapsed milliseconds; negative values are treated as zero.
void rt_animatedsprite2d_update(void *animated_sprite, int64_t delta_ms) {
    rt_animatedsprite2d_impl *impl = rt2d_has_class(animated_sprite, RT2D_ANIMATEDSPRITE_CLASS_ID)
                                         ? (rt_animatedsprite2d_impl *)animated_sprite
                                         : NULL;
    if (!impl || !impl->sprite || !impl->clip || !impl->playing)
        return;
    if (!rt2d_has_class(impl->clip, RT2D_ANIMATIONCLIP_CLASS_ID))
        return;
    rt_animationclip2d_impl *clip = (rt_animationclip2d_impl *)impl->clip;
    int64_t sprite_frames = rt_sprite_get_frame_count(impl->sprite);
    if (sprite_frames <= 0 || clip->start_frame >= sprite_frames) {
        impl->playing = 0;
        impl->frame = 0;
        impl->elapsed_ms = 0;
        return;
    }
    int64_t effective_count = clip->frame_count;
    int64_t available = sprite_frames - clip->start_frame;
    if (effective_count > available)
        effective_count = available;
    if (effective_count <= 0) {
        impl->playing = 0;
        impl->frame = 0;
        impl->elapsed_ms = 0;
        return;
    }
    if (impl->frame < 0)
        impl->frame = 0;
    if (impl->frame >= effective_count)
        impl->frame = clip->loop ? impl->frame % effective_count : effective_count - 1;
    if (delta_ms < 0)
        delta_ms = 0;
    if (delta_ms > INT64_MAX - impl->elapsed_ms)
        impl->elapsed_ms = INT64_MAX;
    else
        impl->elapsed_ms += delta_ms;

    if (impl->elapsed_ms >= clip->frame_delay_ms) {
        int64_t steps = impl->elapsed_ms / clip->frame_delay_ms;
        impl->elapsed_ms %= clip->frame_delay_ms;
        if (clip->loop) {
            steps %= effective_count;
            impl->frame = (impl->frame + steps) % effective_count;
        } else {
            if (steps >= effective_count - impl->frame) {
                impl->frame = effective_count - 1;
                impl->playing = 0;
                impl->elapsed_ms = 0;
            } else {
                impl->frame += steps;
            }
        }
    }
    if (clip->start_frame <= INT64_MAX - impl->frame)
        rt_sprite_set_frame(impl->sprite, clip->start_frame + impl->frame);
}

/// @brief Return the current local frame index within the clip (not the sprite sheet index).
/// @param animated_sprite Borrowed AnimatedSprite2D handle.
/// @return The clip-local frame offset, or `0` for an invalid handle.
int64_t rt_animatedsprite2d_get_frame(void *animated_sprite) {
    return rt2d_has_class(animated_sprite, RT2D_ANIMATEDSPRITE_CLASS_ID)
               ? ((rt_animatedsprite2d_impl *)animated_sprite)->frame
               : 0;
}

/// @brief Return non-zero if the animation is currently playing.
/// @param animated_sprite Borrowed AnimatedSprite2D handle.
/// @return `1` only when playing with a valid sprite and clip, otherwise `0`.
int64_t rt_animatedsprite2d_is_playing(void *animated_sprite) {
    rt_animatedsprite2d_impl *impl = rt2d_has_class(animated_sprite, RT2D_ANIMATEDSPRITE_CLASS_ID)
                                         ? (rt_animatedsprite2d_impl *)animated_sprite
                                         : NULL;
    return impl && impl->playing && impl->sprite &&
                   rt2d_has_class(impl->clip, RT2D_ANIMATIONCLIP_CLASS_ID)
               ? 1
               : 0;
}

/// @brief GC finalizer — releases the retained BitmapFont reference.
/// @param obj TextLayout2D payload being finalized.
static void textlayout2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_TEXTLAYOUT_CLASS_ID))
        return;
    rt_textlayout2d_impl *impl = (rt_textlayout2d_impl *)obj;
    rt2d_release_ref_slot(&impl->font);
}

/// @brief Allocate a TextLayout2D with defaults (scale=1, white, no wrap, left-align).
/// @return A new owned TextLayout2D, or `NULL` on allocation failure.
void *rt_textlayout2d_new(void) {
    rt_textlayout2d_impl *impl = (rt_textlayout2d_impl *)rt_obj_new_i64(
        RT2D_TEXTLAYOUT_CLASS_ID, (int64_t)sizeof(rt_textlayout2d_impl));
    if (!impl)
        return NULL;
    impl->scale = 1;
    impl->color = 0x00FFFFFF;
    rt_obj_set_finalizer(impl, textlayout2d_finalize);
    return impl;
}

/// @brief Bind a BitmapFont, retaining a reference.
/// @param layout Borrowed TextLayout2D handle to update.
/// @param font Borrowed BitmapFont to retain, or `NULL` for the built-in font.
void rt_textlayout2d_set_font(void *layout, void *font) {
    if (rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID) &&
        (!font || rt2d_is_bitmap_font_handle(font)))
        spriterenderer2d_set_ref(&((rt_textlayout2d_impl *)layout)->font, font);
}

/// @brief Set the integer pixel scale; clamped to [1, 64].
/// @param layout Borrowed TextLayout2D handle to update.
/// @param scale Requested integer scale.
void rt_textlayout2d_set_scale(void *layout, int64_t scale) {
    if (rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID))
        ((rt_textlayout2d_impl *)layout)->scale = clamp_i64(scale, 1, 64);
}

/// @brief Set the wrap width in pixels; 0 means no wrapping.
/// @param layout Borrowed TextLayout2D handle to update.
/// @param width Requested wrap width; negative values become zero.
void rt_textlayout2d_set_wrap_width(void *layout, int64_t width) {
    if (rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID))
        ((rt_textlayout2d_impl *)layout)->wrap_width = width < 0 ? 0 : width;
}

/// @brief Set text alignment: 0=left, 1=center, 2=right; clamped to [0, 2].
/// @param layout Borrowed TextLayout2D handle to update.
/// @param alignment Requested alignment value.
void rt_textlayout2d_set_alignment(void *layout, int64_t alignment) {
    if (rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID))
        ((rt_textlayout2d_impl *)layout)->alignment = clamp_i64(alignment, 0, 2);
}

/// @brief Set the text color as 0x00RRGGBB (alpha bits are masked off).
/// @param layout Borrowed TextLayout2D handle to update.
/// @param rgb Packed text color.
void rt_textlayout2d_set_color(void *layout, int64_t rgb) {
    if (rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID))
        ((rt_textlayout2d_impl *)layout)->color = rgb & 0x00FFFFFF;
}

/// @brief Compute the raw pixel width of @p text using the layout's font and scale, without wrap
/// clamping.
/// @details Uses the layout's bitmap font if set; falls back to the canvas default font.
///          Width is multiplied by the layout's scale factor with saturation.
/// @param impl Borrowed layout implementation, or `NULL` for default settings.
/// @param text Borrowed runtime string to measure.
/// @return The scaled unwrapped width, or `0` for a null string.
static int64_t textlayout2d_raw_width(rt_textlayout2d_impl *impl, rt_string text) {
    if (!text)
        return 0;
    int64_t width = impl && impl->font ? rt_bitmapfont_text_width(impl->font, text)
                                       : rt_canvas_text_width(text);
    int64_t scale = impl ? impl->scale : 1;
    return rt2d_saturating_mul_i64(width, scale);
}

/// @brief Scaled height of one text line for the layout's font (px).
/// @param impl Borrowed layout implementation, or `NULL` for default settings.
/// @return The scaled line height with saturating multiplication.
static int64_t textlayout2d_line_height(rt_textlayout2d_impl *impl) {
    int64_t line_height =
        impl && impl->font ? rt_bitmapfont_text_height(impl->font) : rt_canvas_text_height();
    int64_t scale = impl ? impl->scale : 1;
    return rt2d_saturating_mul_i64(line_height, scale);
}

/// @brief Measure the scaled pixel width of a raw byte segment.
/// @details Wraps the bytes in a temporary rt_string, measures via
///          textlayout2d_raw_width, and releases it. Returns 0 for empty input.
/// @param impl Borrowed layout implementation.
/// @param data Borrowed segment bytes.
/// @param len Number of bytes in @p data.
/// @return The segment's scaled pixel width, or `0` for empty input or string
///         allocation failure.
static int64_t textlayout2d_segment_width(rt_textlayout2d_impl *impl,
                                          const char *data,
                                          size_t len) {
    if (!data || len == 0)
        return 0;
    rt_string segment = rt_string_from_bytes(data, len);
    if (!segment)
        return 0;
    int64_t width = textlayout2d_raw_width(impl, segment);
    rt_string_unref(segment);
    return width;
}

typedef struct {
    int64_t max_width;
    int64_t lines;
} rt_textlayout2d_measure;

/// @brief Fold a completed line's width into the running max-width accumulator.
/// @param m In-out measurement accumulator.
/// @param line_width Width of the completed line.
static void textlayout2d_commit_line(rt_textlayout2d_measure *m, int64_t line_width) {
    if (!m)
        return;
    if (line_width > m->max_width)
        m->max_width = line_width;
}

/// @brief Word-wrap accumulator: fit one word onto the current line or wrap.
/// @details Adds @p word (plus any pending inter-word space) to @p line_width.
///          If it would exceed @p wrap_width, commits the current line and
///          starts a new one. Words wider than @p wrap_width are split across
///          multiple lines. Updates @p line_width, @p pending_space_width and
///          the @p measure (max width / line count) in place.
/// @param impl Borrowed layout implementation supplying font and scale.
/// @param word Borrowed word bytes.
/// @param word_len Number of bytes in @p word.
/// @param wrap_width Positive wrap width, or zero to disable wrapping.
/// @param line_width In-out width accumulated on the current line.
/// @param pending_space_width In-out deferred inter-word spacing.
/// @param measure In-out maximum-width and line-count accumulator.
static void textlayout2d_commit_word(rt_textlayout2d_impl *impl,
                                     const char *word,
                                     size_t word_len,
                                     int64_t wrap_width,
                                     int64_t *line_width,
                                     int64_t *pending_space_width,
                                     rt_textlayout2d_measure *measure) {
    if (!word || word_len == 0 || !line_width || !pending_space_width || !measure)
        return;
    int64_t word_width = textlayout2d_segment_width(impl, word, word_len);
    int64_t join_width = *line_width > 0 ? *pending_space_width : 0;
    int64_t candidate = rt2d_saturating_add_i64(*line_width, rt2d_saturating_add_i64(join_width, word_width));
    if (wrap_width > 0 && *line_width > 0 && candidate > wrap_width) {
        textlayout2d_commit_line(measure, *line_width);
        measure->lines = rt2d_saturating_add_i64(measure->lines, 1);
        *line_width = 0;
        join_width = 0;
    }

    if (wrap_width > 0 && word_width > wrap_width) {
        int64_t chunks = rt2d_saturating_add_i64(word_width, wrap_width - 1) / wrap_width;
        if (chunks < 1)
            chunks = 1;
        if (chunks > 1)
            measure->lines = rt2d_saturating_add_i64(measure->lines, chunks - 1);
        if (wrap_width > measure->max_width)
            measure->max_width = wrap_width;
        int64_t rem = word_width % wrap_width;
        *line_width = rem == 0 ? wrap_width : rem;
    } else {
        *line_width = rt2d_saturating_add_i64(*line_width, rt2d_saturating_add_i64(join_width, word_width));
    }
    *pending_space_width = 0;
}

/// @brief Measure wrapped text: compute max line width and total line count.
/// @details Tokenizes @p text on spaces/tabs/newlines, feeding words through
///          textlayout2d_commit_word with the layout's wrap width. Newlines
///          force a line break. Returns {max_width, lines} (lines >= 1).
/// @param impl Borrowed layout implementation, or `NULL` for defaults.
/// @param text Borrowed runtime string to measure.
/// @return Maximum line width and total line count.
static rt_textlayout2d_measure textlayout2d_measure(rt_textlayout2d_impl *impl, rt_string text) {
    rt_textlayout2d_measure measure = {0, 1};
    if (!text)
        return measure;

    const char *data = rt_string_cstr(text);
    size_t len = (size_t)rt_str_len(text);
    int64_t wrap_width = impl ? impl->wrap_width : 0;
    int64_t line_width = 0;
    int64_t pending_space_width = 0;
    size_t word_start = 0;
    size_t word_len = 0;

    for (size_t i = 0; i <= len; i++) {
        char ch = i < len ? data[i] : '\0';
        int is_end = i == len;
        int is_newline = ch == '\n' || ch == '\r';
        int is_space = ch == ' ' || ch == '\t';
        if (!is_end && !is_newline && !is_space) {
            if (word_len == 0)
                word_start = i;
            word_len++;
            continue;
        }

        if (word_len > 0) {
            textlayout2d_commit_word(impl,
                                     data + word_start,
                                     word_len,
                                     wrap_width,
                                     &line_width,
                                     &pending_space_width,
                                     &measure);
            word_len = 0;
        }

        if (is_space) {
            if (line_width > 0)
                pending_space_width = rt2d_saturating_add_i64(
                    pending_space_width, textlayout2d_segment_width(impl, data + i, 1));
            continue;
        }

        if (is_newline) {
            textlayout2d_commit_line(&measure, line_width);
            measure.lines = rt2d_saturating_add_i64(measure.lines, 1);
            line_width = 0;
            pending_space_width = 0;
            if (ch == '\r' && i + 1 < len && data[i + 1] == '\n')
                i++;
        }
    }

    textlayout2d_commit_line(&measure, line_width);
    return measure;
}

/// @brief Measure the display width of `text` respecting explicit newlines and wrap width.
/// @param layout Borrowed TextLayout2D handle, or invalid for default settings.
/// @param text Borrowed runtime string to measure.
/// @return The maximum laid-out line width in pixels.
int64_t rt_textlayout2d_measure_width(void *layout, rt_string text) {
    rt_textlayout2d_impl *impl =
        rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID) ? (rt_textlayout2d_impl *)layout : NULL;
    rt_textlayout2d_measure measure = textlayout2d_measure(impl, text);
    return measure.max_width;
}

/// @brief Measure the total pixel height of `text`, accounting for newlines and word wrapping.
/// @param layout Borrowed TextLayout2D handle, or invalid for default settings.
/// @param text Borrowed runtime string to measure.
/// @return The saturating product of line height and computed line count.
int64_t rt_textlayout2d_measure_height(void *layout, rt_string text) {
    rt_textlayout2d_impl *impl =
        rt2d_has_class(layout, RT2D_TEXTLAYOUT_CLASS_ID) ? (rt_textlayout2d_impl *)layout : NULL;
    rt_textlayout2d_measure measure = textlayout2d_measure(impl, text);
    return rt2d_saturating_mul_i64(textlayout2d_line_height(impl), measure.lines);
}

/// @brief GC finalizer — releases retained references to source, target, and shader.
/// @param obj RenderPass2D payload being finalized.
static void renderpass2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_RENDERPASS_CLASS_ID))
        return;
    rt_renderpass2d_impl *impl = (rt_renderpass2d_impl *)obj;
    rt2d_release_ref_slot(&impl->source);
    rt2d_release_ref_slot(&impl->target);
    rt2d_release_ref_slot(&impl->shader);
}

/// @brief Allocate a RenderPass2D that reads from `source` and writes to `target`.
/// @details Both references must be valid RenderTarget2D or Surface2D handles
///          and are retained. The pass starts enabled. A Shader2D can be
///          attached later via `SetShader`; without one, source is cloned
///          directly into target (pixel-perfect copy).
/// @param source Borrowed valid render surface retained as input.
/// @param target Borrowed valid render surface retained as output.
/// @return A new owned RenderPass2D, or `NULL` for invalid surfaces or
///         allocation failure.
void *rt_renderpass2d_new(void *source, void *target) {
    if (!rendertarget2d_checked(source) || !rendertarget2d_checked(target))
        return NULL;
    rt_renderpass2d_impl *impl = (rt_renderpass2d_impl *)rt_obj_new_i64(
        RT2D_RENDERPASS_CLASS_ID, (int64_t)sizeof(rt_renderpass2d_impl));
    if (!impl)
        return NULL;
    impl->source = NULL;
    impl->target = NULL;
    impl->shader = NULL;
    impl->enabled = 0;
    rt_obj_set_finalizer(impl, renderpass2d_finalize);
    if (source) {
        rt2d_retain_ref(source);
        impl->source = source;
    }
    if (target) {
        rt2d_retain_ref(target);
        impl->target = target;
    }
    impl->enabled = 1;
    return impl;
}

/// @brief Replace the source RenderTarget2D, retaining a reference to the new one.
/// @param pass Borrowed RenderPass2D handle to update.
/// @param source Borrowed valid render surface to retain, or `NULL`.
void rt_renderpass2d_set_source(void *pass, void *source) {
    if (rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID) &&
        (!source || rendertarget2d_checked(source)))
        spriterenderer2d_set_ref(&((rt_renderpass2d_impl *)pass)->source, source);
}

/// @brief Replace the target RenderTarget2D, retaining a reference to the new one.
/// @param pass Borrowed RenderPass2D handle to update.
/// @param target Borrowed valid render surface to retain, or `NULL`.
void rt_renderpass2d_set_target(void *pass, void *target) {
    if (rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID) &&
        (!target || rendertarget2d_checked(target)))
        spriterenderer2d_set_ref(&((rt_renderpass2d_impl *)pass)->target, target);
}

/// @brief Bind a Shader2D for this pass, retaining a reference.
/// @param pass Borrowed RenderPass2D handle to update.
/// @param shader Borrowed Shader2D or PostProcess2D to retain, or `NULL`.
void rt_renderpass2d_set_shader(void *pass, void *shader) {
    if (rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID) && (!shader || rt2d_is_shader_like(shader)))
        spriterenderer2d_set_ref(&((rt_renderpass2d_impl *)pass)->shader, shader);
}

/// @brief Enable or disable this pass (disabled passes are skipped by `Execute`).
/// @param pass Borrowed RenderPass2D handle to update.
/// @param enabled Nonzero to enable the pass.
void rt_renderpass2d_set_enabled(void *pass, int64_t enabled) {
    if (rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID))
        ((rt_renderpass2d_impl *)pass)->enabled = enabled != 0;
}

/// @brief Return non-zero if this pass is currently enabled.
/// @param pass Borrowed RenderPass2D handle.
/// @return `1` when enabled, otherwise `0`.
int64_t rt_renderpass2d_get_enabled(void *pass) {
    return rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID) ? ((rt_renderpass2d_impl *)pass)->enabled
                                                          : 0;
}

/// @brief Execute this pass: apply the optional shader and blit the result to target.
/// @details No-op if disabled, or if source/target are NULL or not valid render targets.
///          If a Shader2D is bound, applies it via `rt_shader2d_apply`; otherwise
///          clones the source pixels directly. The target is cleared before writing.
/// @param pass Borrowed RenderPass2D handle to execute.
void rt_renderpass2d_execute(void *pass) {
    rt_renderpass2d_impl *impl =
        rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID) ? (rt_renderpass2d_impl *)pass : NULL;
    if (!impl || !impl->enabled || !impl->source || !impl->target)
        return;
    if (!rendertarget2d_checked(impl->source) || !rendertarget2d_checked(impl->target))
        return;
    if (impl->shader && !rt2d_is_shader_like(impl->shader))
        return;
    void *source_pixels = rt_rendertarget2d_get_pixels(impl->source);
    if (!source_pixels)
        return;
    void *processed = NULL;
    if (impl->shader) {
        if (shader2d_checked(impl->shader))
            processed = rt_shader2d_apply(impl->shader, source_pixels);
        else
            processed = rt_postprocess2d_apply(impl->shader, source_pixels);
    } else {
        processed = rt_pixels_clone(source_pixels);
    }
    if (!processed)
        return;
    rt_rendertarget2d_clear(impl->target, 0);
    rt_rendertarget2d_draw_pixels(impl->target, 0, 0, processed);
    rt2d_release_ref_slot(&processed);
}

/// @brief GC finalizer — releases all retained pass references and frees the passes array.
/// @param obj RenderGraph2D payload being finalized.
static void rendergraph2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_RENDERGRAPH_CLASS_ID))
        return;
    rt_rendergraph2d_impl *impl = (rt_rendergraph2d_impl *)obj;
    if (!impl)
        return;
    for (int64_t i = 0; i < impl->count; i++)
        rt2d_release_ref_slot(&impl->passes[i]);
    free(impl->passes);
}

/// @brief Allocate a RenderGraph2D with the given initial pass-array capacity.
/// @param capacity Requested initial pass capacity.
/// @return A new owned RenderGraph2D, or `NULL` on allocation failure.
void *rt_rendergraph2d_new(int64_t capacity) {
    rt_rendergraph2d_impl *impl = (rt_rendergraph2d_impl *)rt_obj_new_i64(
        RT2D_RENDERGRAPH_CLASS_ID, (int64_t)sizeof(rt_rendergraph2d_impl));
    if (!impl)
        return NULL;
    impl->capacity = rt2d_initial_capacity(capacity);
    impl->passes = (void **)calloc((size_t)impl->capacity, sizeof(void *));
    if (!impl->passes) {
        if (rt_obj_release_check0(impl))
            rt_obj_free(impl);
        return NULL;
    }
    rt_obj_set_finalizer(impl, rendergraph2d_finalize);
    return impl;
}

/// @brief Grow the render-pass array to fit at least `needed` entries.
/// @details Same doubling-growth + overflow-guard pattern as the other `_reserve`
///          helpers in this file (renderer / path / object-layer). The new tail
///          is zero-initialized so unused slots never contain stale pointers.
/// @param impl Borrowed graph implementation to grow.
/// @param needed Minimum required pass capacity.
/// @return 1 on success, 0 on overflow or allocation failure.
static int32_t rendergraph2d_reserve(rt_rendergraph2d_impl *impl, int64_t needed) {
    if (!impl || needed <= impl->capacity)
        return 1;
    int64_t cap = impl->capacity > 0 ? impl->capacity : RT2D_INITIAL_CAP;
    while (cap < needed) {
        if (cap > INT64_MAX / 2)
            return 0;
        cap *= 2;
    }
    if (cap > INT64_MAX / (int64_t)sizeof(void *))
        return 0;
    void **passes = (void **)realloc(impl->passes, (size_t)cap * sizeof(void *));
    if (!passes)
        return 0;
    memset(passes + impl->capacity, 0, (size_t)(cap - impl->capacity) * sizeof(void *));
    impl->passes = passes;
    impl->capacity = cap;
    return 1;
}

/// @brief Append a RenderPass2D to the graph, retaining a reference to it.
/// @details Grows the backing array if needed. Traps on capacity overflow.
/// @param graph Borrowed RenderGraph2D receiving the pass.
/// @param pass Borrowed valid RenderPass2D to retain.
void rt_rendergraph2d_add_pass(void *graph, void *pass) {
    rt_rendergraph2d_impl *impl =
        rt2d_has_class(graph, RT2D_RENDERGRAPH_CLASS_ID) ? (rt_rendergraph2d_impl *)graph : NULL;
    if (!impl || !rt2d_has_class(pass, RT2D_RENDERPASS_CLASS_ID))
        return;
    if (!rendergraph2d_reserve(impl, impl->count + 1)) {
        rt_trap("RenderGraph2D: capacity overflow");
        return;
    }
    rt2d_retain_ref(pass);
    impl->passes[impl->count++] = pass;
}

/// @brief Remove all passes from the graph, releasing their retained references.
/// @param graph Borrowed RenderGraph2D to clear.
void rt_rendergraph2d_clear(void *graph) {
    rt_rendergraph2d_impl *impl =
        rt2d_has_class(graph, RT2D_RENDERGRAPH_CLASS_ID) ? (rt_rendergraph2d_impl *)graph : NULL;
    if (!impl)
        return;
    for (int64_t i = 0; i < impl->count; i++)
        rt2d_release_ref_slot(&impl->passes[i]);
    impl->count = 0;
}

/// @brief Return the number of passes currently in the graph.
/// @param graph Borrowed RenderGraph2D handle.
/// @return The pass count, or `0` for an invalid handle.
int64_t rt_rendergraph2d_get_count(void *graph) {
    return rt2d_has_class(graph, RT2D_RENDERGRAPH_CLASS_ID)
               ? ((rt_rendergraph2d_impl *)graph)->count
               : 0;
}

/// @brief Execute every pass in insertion order.
/// @details Each pass is executed unconditionally here — individual passes skip
///          themselves via their `enabled` flag inside `rt_renderpass2d_execute`.
/// @param graph Borrowed RenderGraph2D to execute.
void rt_rendergraph2d_execute(void *graph) {
    rt_rendergraph2d_impl *impl =
        rt2d_has_class(graph, RT2D_RENDERGRAPH_CLASS_ID) ? (rt_rendergraph2d_impl *)graph : NULL;
    if (!impl)
        return;
    for (int64_t i = 0; i < impl->count; i++)
        rt_renderpass2d_execute(impl->passes[i]);
}

/// @brief GC finalizer — frees the byte-per-cell solidity grid.
/// @param obj CollisionMask2D payload being finalized.
static void collisionmask2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_COLLISIONMASK_CLASS_ID))
        return;
    rt_collisionmask2d_impl *impl = (rt_collisionmask2d_impl *)obj;
    free(impl->bits);
}

/// @brief Allocate a CollisionMask2D with all cells initially clear (solid=0).
/// @details `rt2d_checked_count` guards against dimension overflow; traps with a message
///          if `width * height` overflows. One byte per cell — not a true bit-pack,
///          but faster to read with no bit-shift math.
/// @param width Positive mask width in cells.
/// @param height Positive mask height in cells.
/// @return A new owned CollisionMask2D, or `NULL` on allocation failure; invalid
///         dimensions trap and return `NULL`.
void *rt_collisionmask2d_new(int64_t width, int64_t height) {
    int64_t count = 0;
    if (!rt2d_checked_count(width, height, 1, &count)) {
        rt_trap("CollisionMask2D.New: invalid dimensions");
        return NULL;
    }
    rt_collisionmask2d_impl *impl = (rt_collisionmask2d_impl *)rt_obj_new_i64(
        RT2D_COLLISIONMASK_CLASS_ID, (int64_t)sizeof(rt_collisionmask2d_impl));
    if (!impl)
        return NULL;
    impl->bits = (uint8_t *)calloc((size_t)count, 1);
    if (!impl->bits) {
        if (rt_obj_release_check0(impl))
            rt_obj_free(impl);
        return NULL;
    }
    impl->width = width;
    impl->height = height;
    rt_obj_set_finalizer(impl, collisionmask2d_finalize);
    return impl;
}

/// @brief Build a CollisionMask2D by classifying each pixel as solid/empty by alpha.
/// @details Pixels with alpha >= `alpha_threshold` (clamped to [0, 255]) are marked
///          solid. Useful for creating a per-pixel collision shape from a sprite's
///          alpha channel without any manual tilemap authoring. A threshold of 0
///          means "any non-zero alpha" so fully transparent pixels stay empty.
/// @param pixels Borrowed valid source Pixels image.
/// @param alpha_threshold Requested alpha cutoff.
/// @return A new owned CollisionMask2D, or `NULL` for invalid input or allocation failure.
void *rt_collisionmask2d_from_pixels(void *pixels, int64_t alpha_threshold) {
    rt_pixels_impl *pix = rt_pixels_checked_impl_or_null(pixels);
    if (!pix || pix->width <= 0 || pix->height <= 0 || !pix->data)
        return NULL;
    int64_t width = pix->width;
    int64_t height = pix->height;
    void *mask = rt_collisionmask2d_new(width, height);
    if (!mask)
        return NULL;
    int64_t threshold = clamp_u8_i64(alpha_threshold);
    for (int64_t y = 0; y < height; y++) {
        for (int64_t x = 0; x < width; x++) {
            uint32_t alpha = pix->data[y * width + x] & 255u;
            int64_t solid = threshold <= 0 ? alpha > 0u : alpha >= (uint32_t)threshold;
            rt_collisionmask2d_set(mask, x, y, solid);
        }
    }
    return mask;
}

/// @brief Return the width of the mask in cells.
/// @param mask Borrowed CollisionMask2D handle.
/// @return The width, or `0` for an invalid handle.
int64_t rt_collisionmask2d_get_width(void *mask) {
    return rt2d_has_class(mask, RT2D_COLLISIONMASK_CLASS_ID)
               ? ((rt_collisionmask2d_impl *)mask)->width
               : 0;
}

/// @brief Return the height of the mask in cells.
/// @param mask Borrowed CollisionMask2D handle.
/// @return The height, or `0` for an invalid handle.
int64_t rt_collisionmask2d_get_height(void *mask) {
    return rt2d_has_class(mask, RT2D_COLLISIONMASK_CLASS_ID)
               ? ((rt_collisionmask2d_impl *)mask)->height
               : 0;
}

/// @brief Return non-zero if cell (x, y) is within the collision mask's bounds.
/// @param impl Borrowed collision-mask implementation.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @return Nonzero when both coordinates lie inside the mask.
static int32_t collisionmask2d_in_bounds(rt_collisionmask2d_impl *impl, int64_t x, int64_t y) {
    return impl && x >= 0 && y >= 0 && x < impl->width && y < impl->height;
}

/// @brief Set or clear the solid flag at `(x, y)`; out-of-range calls are silently dropped.
/// @param mask Borrowed CollisionMask2D handle to update.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @param solid Nonzero to mark solid, zero to clear.
void rt_collisionmask2d_set(void *mask, int64_t x, int64_t y, int64_t solid) {
    rt_collisionmask2d_impl *impl =
        rt2d_has_class(mask, RT2D_COLLISIONMASK_CLASS_ID) ? (rt_collisionmask2d_impl *)mask : NULL;
    if (!collisionmask2d_in_bounds(impl, x, y))
        return;
    impl->bits[y * impl->width + x] = solid != 0;
}

/// @brief Return 1 if cell `(x, y)` is solid, 0 if empty or out of range.
/// @param mask Borrowed CollisionMask2D handle.
/// @param x Cell X coordinate.
/// @param y Cell Y coordinate.
/// @return `1` for a solid in-bounds cell, otherwise `0`.
int64_t rt_collisionmask2d_get(void *mask, int64_t x, int64_t y) {
    rt_collisionmask2d_impl *impl =
        rt2d_has_class(mask, RT2D_COLLISIONMASK_CLASS_ID) ? (rt_collisionmask2d_impl *)mask : NULL;
    if (!collisionmask2d_in_bounds(impl, x, y))
        return 0;
    return impl->bits[y * impl->width + x] ? 1 : 0;
}

/// @brief Pixel-perfect overlap test between two masks at world positions `(ax, ay)` and `(bx,
/// by)`.
/// @details Computes the axis-aligned intersection rectangle, then walks every cell
///          in that rectangle checking whether both masks are solid — returns 1 on the
///          first matching solid pair, 0 if no overlap exists. Early-exits if the AABBs
///          don't intersect at all.
/// @param a Borrowed first CollisionMask2D.
/// @param ax World X coordinate of @p a's top-left cell.
/// @param ay World Y coordinate of @p a's top-left cell.
/// @param b Borrowed second CollisionMask2D.
/// @param bx World X coordinate of @p b's top-left cell.
/// @param by World Y coordinate of @p b's top-left cell.
/// @return `1` if the positioned masks share a solid cell, otherwise `0`.
int64_t rt_collisionmask2d_overlaps(
    void *a, int64_t ax, int64_t ay, void *b, int64_t bx, int64_t by) {
    rt_collisionmask2d_impl *ma =
        rt2d_has_class(a, RT2D_COLLISIONMASK_CLASS_ID) ? (rt_collisionmask2d_impl *)a : NULL;
    rt_collisionmask2d_impl *mb =
        rt2d_has_class(b, RT2D_COLLISIONMASK_CLASS_ID) ? (rt_collisionmask2d_impl *)b : NULL;
    if (!ma || !mb)
        return 0;
    int64_t left = ax > bx ? ax : bx;
    int64_t top = ay > by ? ay : by;
    if (!point_in_interval_i64(ax, ma->width, left) ||
        !point_in_interval_i64(bx, mb->width, left) ||
        !point_in_interval_i64(ay, ma->height, top) || !point_in_interval_i64(by, mb->height, top))
        return 0;

    uint64_t ax0 = (uint64_t)left - (uint64_t)ax;
    uint64_t bx0 = (uint64_t)left - (uint64_t)bx;
    uint64_t ay0 = (uint64_t)top - (uint64_t)ay;
    uint64_t by0 = (uint64_t)top - (uint64_t)by;
    uint64_t overlap_w = (uint64_t)ma->width - ax0;
    uint64_t b_rem_w = (uint64_t)mb->width - bx0;
    if (b_rem_w < overlap_w)
        overlap_w = b_rem_w;
    uint64_t overlap_h = (uint64_t)ma->height - ay0;
    uint64_t b_rem_h = (uint64_t)mb->height - by0;
    if (b_rem_h < overlap_h)
        overlap_h = b_rem_h;

    /* Both handles are validated above and the loop bounds are the guaranteed
     * intersection, so index ma->bits / mb->bits directly instead of paying a
     * per-cell handle re-validation (rt_collisionmask2d_get) — this runs every
     * frame per entity pair and was ~O(w*h) redundant class checks. */
    for (uint64_t dy = 0; dy < overlap_h; dy++) {
        const uint8_t *arow = &ma->bits[(ay0 + dy) * (uint64_t)ma->width + ax0];
        const uint8_t *brow = &mb->bits[(by0 + dy) * (uint64_t)mb->width + bx0];
        for (uint64_t dx = 0; dx < overlap_w; dx++) {
            if (arow[dx] && brow[dx])
                return 1;
        }
    }
    return 0;
}

/// @brief Allocate a Hitbox2D at `(x, y)` with the given dimensions (negative sizes clamped to 0).
/// @param x Top-left X coordinate.
/// @param y Top-left Y coordinate.
/// @param width Requested width.
/// @param height Requested height.
/// @return A new owned HitBox2D, or `NULL` on allocation failure.
void *rt_hitbox2d_new(int64_t x, int64_t y, int64_t width, int64_t height) {
    rt_hitbox2d_impl *impl =
        (rt_hitbox2d_impl *)rt_obj_new_i64(RT2D_HITBOX_CLASS_ID, (int64_t)sizeof(rt_hitbox2d_impl));
    if (!impl)
        return NULL;
    rt_hitbox2d_set(impl, x, y, width, height);
    return impl;
}

/// @brief Reposition and resize an existing hitbox in one call.
/// @param hitbox Borrowed HitBox2D handle to update.
/// @param x New top-left X coordinate.
/// @param y New top-left Y coordinate.
/// @param width New width; negative values become zero.
/// @param height New height; negative values become zero.
void rt_hitbox2d_set(void *hitbox, int64_t x, int64_t y, int64_t width, int64_t height) {
    if (!rt2d_has_class(hitbox, RT2D_HITBOX_CLASS_ID))
        return;
    rt_hitbox2d_impl *impl = (rt_hitbox2d_impl *)hitbox;
    impl->x = x;
    impl->y = y;
    impl->width = width < 0 ? 0 : width;
    impl->height = height < 0 ? 0 : height;
}

/// @brief Return the X world-position of the hitbox's top-left corner.
/// @param hitbox Borrowed HitBox2D handle.
/// @return The X coordinate, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_x(void *hitbox) {
    return rt2d_has_class(hitbox, RT2D_HITBOX_CLASS_ID) ? ((rt_hitbox2d_impl *)hitbox)->x : 0;
}

/// @brief Return the Y world-position of the hitbox's top-left corner.
/// @param hitbox Borrowed HitBox2D handle.
/// @return The Y coordinate, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_y(void *hitbox) {
    return rt2d_has_class(hitbox, RT2D_HITBOX_CLASS_ID) ? ((rt_hitbox2d_impl *)hitbox)->y : 0;
}

/// @brief Return the width of the hitbox in pixels.
/// @param hitbox Borrowed HitBox2D handle.
/// @return The nonnegative width, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_width(void *hitbox) {
    return rt2d_has_class(hitbox, RT2D_HITBOX_CLASS_ID) ? ((rt_hitbox2d_impl *)hitbox)->width : 0;
}

/// @brief Return the height of the hitbox in pixels.
/// @param hitbox Borrowed HitBox2D handle.
/// @return The nonnegative height, or `0` for an invalid handle.
int64_t rt_hitbox2d_get_height(void *hitbox) {
    return rt2d_has_class(hitbox, RT2D_HITBOX_CLASS_ID) ? ((rt_hitbox2d_impl *)hitbox)->height : 0;
}

/// @brief Return non-zero if point `(x, y)` is inside this hitbox.
/// @details Uses `point_in_interval_i64` for both axes: [x, x+width) × [y, y+height).
/// @param hitbox Borrowed HitBox2D handle.
/// @param x Point X coordinate.
/// @param y Point Y coordinate.
/// @return Nonzero when the point lies in both half-open intervals.
int64_t rt_hitbox2d_contains(void *hitbox, int64_t x, int64_t y) {
    rt_hitbox2d_impl *impl =
        rt2d_has_class(hitbox, RT2D_HITBOX_CLASS_ID) ? (rt_hitbox2d_impl *)hitbox : NULL;
    return impl && point_in_interval_i64(impl->x, impl->width, x) &&
           point_in_interval_i64(impl->y, impl->height, y);
}

/// @brief Return non-zero if two hitboxes overlap (AABB vs AABB test).
/// @details Uses `intervals_overlap_i64` on both axes — no separation means overlap.
/// @param a Borrowed first HitBox2D.
/// @param b Borrowed second HitBox2D.
/// @return Nonzero for positive-area overlap; invalid handles and edge-only
///         contact return zero.
int64_t rt_hitbox2d_intersects(void *a, void *b) {
    rt_hitbox2d_impl *ha = rt2d_has_class(a, RT2D_HITBOX_CLASS_ID) ? (rt_hitbox2d_impl *)a : NULL;
    rt_hitbox2d_impl *hb = rt2d_has_class(b, RT2D_HITBOX_CLASS_ID) ? (rt_hitbox2d_impl *)b : NULL;
    if (!ha || !hb)
        return 0;
    return intervals_overlap_i64(ha->x, ha->width, hb->x, hb->width) &&
           intervals_overlap_i64(ha->y, ha->height, hb->y, hb->height);
}

/// @brief Allocate an empty Palette2D (all 256 slots zeroed, count=0).
/// @return A new owned Palette2D, or `NULL` on allocation failure.
void *rt_palette2d_new(void) {
    rt_palette2d_impl *impl = (rt_palette2d_impl *)rt_obj_new_i64(
        RT2D_PALETTE_CLASS_ID, (int64_t)sizeof(rt_palette2d_impl));
    if (!impl)
        return NULL;
    return impl;
}

/// @brief Set a palette entry at `index` [0, 255] to `rgba`.
/// @details `count` is extended to `index+1` if the new index is the highest seen so far.
/// @param palette Borrowed Palette2D handle to update.
/// @param index Slot index in `0..255`.
/// @param rgba Raw RGBA or tagged Color value to normalize and store.
void rt_palette2d_set_color(void *palette, int64_t index, int64_t rgba) {
    rt_palette2d_impl *impl =
        rt2d_has_class(palette, RT2D_PALETTE_CLASS_ID) ? (rt_palette2d_impl *)palette : NULL;
    if (!impl || index < 0 || index >= 256)
        return;
    impl->colors[index] = rt_pixels_rgba_or_tagged_color_to_rgba(rgba);
    impl->defined[index] = 1;
    if (index + 1 > impl->count)
        impl->count = index + 1;
}

/// @brief Return the RGBA color at `index`, or 0 if out of range or beyond `count`.
/// @param palette Borrowed Palette2D handle.
/// @param index Slot index to query.
/// @return The defined raw `0xRRGGBBAA` color, or `0` when undefined or invalid.
int64_t rt_palette2d_get_color(void *palette, int64_t index) {
    rt_palette2d_impl *impl =
        rt2d_has_class(palette, RT2D_PALETTE_CLASS_ID) ? (rt_palette2d_impl *)palette : NULL;
    if (!impl || index < 0 || index >= 256 || !impl->defined[index])
        return 0;
    return impl->colors[index];
}

/// @brief Return the palette entry as a Color.RGBA-compatible value.
/// @param palette Borrowed Palette2D handle.
/// @param index Slot index to query.
/// @return A tagged Color representation of the raw lookup result.
int64_t rt_palette2d_get_color_value(void *palette, int64_t index) {
    return rt_pixels_rgba_to_color((uint32_t)rt_palette2d_get_color(palette, index));
}

/// @brief Return the number of palette entries set (highest set index + 1).
/// @param palette Borrowed Palette2D handle.
/// @return One greater than the highest defined index, or `0` if empty or invalid.
int64_t rt_palette2d_get_count(void *palette) {
    return rt2d_has_class(palette, RT2D_PALETTE_CLASS_ID) ? ((rt_palette2d_impl *)palette)->count
                                                          : 0;
}

/// @brief Remap indexed pixels through a palette using the red byte as the index.
/// @details Shared implementation behind the compatibility ApplyLegacy path;
///          returns a new owned Pixels image.
/// @param palette Borrowed Palette2D supplying indexed colors.
/// @param pixels Borrowed source Pixels image.
/// @param legacy_alpha_index Nonzero to interpret `0x000000II` alpha bytes as indices.
/// @return A new owned remapped Pixels image, or `NULL` for invalid input or
///         allocation failure.
static void *palette2d_apply_indexed(void *palette, void *pixels, int8_t legacy_alpha_index) {
    rt_palette2d_impl *impl =
        rt2d_has_class(palette, RT2D_PALETTE_CLASS_ID) ? (rt_palette2d_impl *)palette : NULL;
    rt_pixels_impl *src_pixels = rt_pixels_checked_impl_or_null(pixels);
    if (!impl || !src_pixels || !src_pixels->data)
        return NULL;
    int64_t width = src_pixels->width;
    int64_t height = src_pixels->height;
    void *out = rt_pixels_new(width, height);
    if (!out)
        return NULL;
    rt_pixels_impl *dst_pixels = rt_pixels_checked_impl_or_null(out);
    if (!dst_pixels || !dst_pixels->data) {
        rt2d_release_ref_slot(&out);
        return NULL;
    }
    for (int64_t y = 0; y < height; y++) {
        for (int64_t x = 0; x < width; x++) {
            uint32_t src = src_pixels->data[y * width + x];
            uint8_t alpha = (uint8_t)(src & 255u);
            int64_t index = (legacy_alpha_index && alpha != 255u && (src & 0xFFFFFF00u) == 0u)
                                ? (int64_t)alpha
                                : (int64_t)((src >> 24) & 255u);
            uint32_t mapped =
                (index >= 0 && index < 256 && impl->defined[index]) ? impl->colors[index] : src;
            dst_pixels->data[y * width + x] = mapped;
        }
    }
    return out;
}

/// @brief Remap a Pixels image through this palette and return a new Pixels.
/// @details For each pixel, selects the nearest defined palette color in RGBA
///          space. Unset palette slots are ignored; an empty palette returns a
///          clone. Returns NULL on invalid input/allocation failure.
/// @param palette Borrowed Palette2D supplying candidate colors.
/// @param pixels Borrowed source Pixels image.
/// @return A new owned nearest-color Pixels image, or `NULL` on failure.
void *rt_palette2d_apply(void *palette, void *pixels) {
    rt_palette2d_impl *impl =
        rt2d_has_class(palette, RT2D_PALETTE_CLASS_ID) ? (rt_palette2d_impl *)palette : NULL;
    rt_pixels_impl *src_pixels = rt_pixels_checked_impl_or_null(pixels);
    if (!impl || !src_pixels || !src_pixels->data)
        return NULL;

    void *out = rt_pixels_new(src_pixels->width, src_pixels->height);
    if (!out)
        return NULL;
    rt_pixels_impl *dst_pixels = rt_pixels_checked_impl_or_null(out);
    if (!dst_pixels || !dst_pixels->data) {
        rt2d_release_ref_slot(&out);
        return NULL;
    }

    for (int64_t y = 0; y < src_pixels->height; y++) {
        for (int64_t x = 0; x < src_pixels->width; x++) {
            uint32_t src = src_pixels->data[y * src_pixels->width + x];
            uint32_t best = src;
            uint64_t best_distance = UINT64_MAX;
            for (int64_t i = 0; i < impl->count && i < 256; i++) {
                if (!impl->defined[i])
                    continue;
                uint32_t candidate = impl->colors[i];
                int64_t dr = (int64_t)((src >> 24) & 255u) - (int64_t)((candidate >> 24) & 255u);
                int64_t dg = (int64_t)((src >> 16) & 255u) - (int64_t)((candidate >> 16) & 255u);
                int64_t db = (int64_t)((src >> 8) & 255u) - (int64_t)((candidate >> 8) & 255u);
                int64_t da = (int64_t)(src & 255u) - (int64_t)(candidate & 255u);
                uint64_t distance = (uint64_t)(dr * dr) + (uint64_t)(dg * dg) +
                                    (uint64_t)(db * db) + (uint64_t)(da * da);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = candidate;
                }
            }
            dst_pixels->data[y * src_pixels->width + x] = best;
        }
    }
    return out;
}

/// @brief Legacy indexed remap that also accepts `0x000000II` alpha-byte indices.
/// @details Kept separate from Apply so normal transparent black pixels are no longer
///          interpreted as palette index 128 or similar by accident.
/// @param palette Borrowed Palette2D supplying indexed colors.
/// @param pixels Borrowed source Pixels image.
/// @return A new owned indexed-remap Pixels image, or `NULL` on failure.
void *rt_palette2d_apply_legacy(void *palette, void *pixels) {
    return palette2d_apply_indexed(palette, pixels, 1);
}

/// @brief Linear interpolation between two RGBA colors at `t_pct` percent [0, 100].
/// @details Each channel is interpolated independently in integer arithmetic:
///          `result_ch = a_ch + (b_ch - a_ch) * t / 100`. No gamma correction —
///          interpolation happens in gamma-encoded space, matching the simpler
///          expectation of the 2D tool palette.
/// @param a Raw `0xRRGGBBAA` start color.
/// @param b Raw `0xRRGGBBAA` end color.
/// @param t_pct Interpolation percentage clamped to `0..100`.
/// @return The channel-wise interpolated raw RGBA color.
static uint32_t lerp_rgba(uint32_t a, uint32_t b, int64_t t_pct) {
    int64_t t = clamp_i64(t_pct, 0, 100);
    int64_t ar = (a >> 24) & 255;
    int64_t ag = (a >> 16) & 255;
    int64_t ab = (a >> 8) & 255;
    int64_t aa = a & 255;
    int64_t br = (b >> 24) & 255;
    int64_t bg = (b >> 16) & 255;
    int64_t bb = (b >> 8) & 255;
    int64_t ba = b & 255;
    int64_t r = ar + (br - ar) * t / 100;
    int64_t g = ag + (bg - ag) * t / 100;
    int64_t blue = ab + (bb - ab) * t / 100;
    int64_t alpha = aa + (ba - aa) * t / 100;
    return (uint32_t)((r << 24) | (g << 16) | (blue << 8) | alpha);
}

/// @brief Allocate a Gradient2D with the given start/end RGBA colors and step count.
/// @details `steps` is the number of quantization levels; 2 means a smooth linear
///          gradient, higher values produce visible banding. Non-positive steps
///          default to 2. Colors are normalized via `rt_pixels_rgba_or_tagged_color_to_rgba`.
/// @param start_rgba Raw RGBA or tagged Color start value.
/// @param end_rgba Raw RGBA or tagged Color end value.
/// @param steps Requested quantization level count.
/// @return A new owned Gradient2D, or `NULL` on allocation failure.
void *rt_gradient2d_new(int64_t start_rgba, int64_t end_rgba, int64_t steps) {
    rt_gradient2d_impl *impl = (rt_gradient2d_impl *)rt_obj_new_i64(
        RT2D_GRADIENT_CLASS_ID, (int64_t)sizeof(rt_gradient2d_impl));
    if (!impl)
        return NULL;
    impl->start = rt_pixels_rgba_or_tagged_color_to_rgba(start_rgba);
    impl->end = rt_pixels_rgba_or_tagged_color_to_rgba(end_rgba);
    impl->steps = steps <= 0 ? 2 : steps;
    return impl;
}

/// @brief Update the start and end colors of an existing gradient.
/// @param gradient Borrowed Gradient2D handle to update.
/// @param start_rgba Raw RGBA or tagged Color start value.
/// @param end_rgba Raw RGBA or tagged Color end value.
void rt_gradient2d_set_colors(void *gradient, int64_t start_rgba, int64_t end_rgba) {
    if (!rt2d_has_class(gradient, RT2D_GRADIENT_CLASS_ID))
        return;
    rt_gradient2d_impl *impl = (rt_gradient2d_impl *)gradient;
    impl->start = rt_pixels_rgba_or_tagged_color_to_rgba(start_rgba);
    impl->end = rt_pixels_rgba_or_tagged_color_to_rgba(end_rgba);
}

/// @brief Set the step count; non-positive values reset to 2 (smooth).
/// @param gradient Borrowed Gradient2D handle to update.
/// @param steps Requested quantization level count.
void rt_gradient2d_set_steps(void *gradient, int64_t steps) {
    if (rt2d_has_class(gradient, RT2D_GRADIENT_CLASS_ID))
        ((rt_gradient2d_impl *)gradient)->steps = steps <= 0 ? 2 : steps;
}

/// @brief Sample the gradient at position `t_pct` percent [0, 100] and return RGBA.
/// @details `steps <= 2` means a smooth gradient. Larger values quantize `t`
///          into that many buckets before interpolation. Steps above 101 are
///          clamped to keep the bucket arithmetic safe.
/// @param gradient Borrowed Gradient2D handle.
/// @param t_pct Sample position clamped to `0..100`.
/// @return The sampled raw `0xRRGGBBAA` color, or `0` for an invalid handle.
int64_t rt_gradient2d_sample(void *gradient, int64_t t_pct) {
    rt_gradient2d_impl *impl =
        rt2d_has_class(gradient, RT2D_GRADIENT_CLASS_ID) ? (rt_gradient2d_impl *)gradient : NULL;
    if (!impl)
        return 0;
    int64_t t = clamp_i64(t_pct, 0, 100);
    int64_t steps = impl->steps < 2 ? 2 : impl->steps;
    if (steps > 101)
        steps = 101;
    if (steps > 2) {
        int64_t bucket = (t * (steps - 1) + 50) / 100;
        t = bucket * 100 / (steps - 1);
    }
    return lerp_rgba(impl->start, impl->end, t);
}

/// @brief Sample the gradient and return a Color.RGBA-compatible value.
/// @param gradient Borrowed Gradient2D handle.
/// @param t_pct Sample position clamped to `0..100`.
/// @return The sample converted to tagged Color representation.
int64_t rt_gradient2d_sample_color(void *gradient, int64_t t_pct) {
    return rt_pixels_rgba_to_color((uint32_t)rt_gradient2d_sample(gradient, t_pct));
}

/// @brief Convert documented normalized sample positions to the internal percent scale.
/// @details Values in [0.0, 1.0] are normalized positions. Values above 1.0 are
///          treated as legacy percent inputs so old `50` callers keep sampling
///          the midpoint after the public binding moves to Number.
/// @param t Normalized position or legacy percentage.
/// @return Rounded percentage clamped to `0..100`; NaN maps to zero.
static int64_t gradient2d_position_to_percent(double t) {
    if (t != t)
        return 0;
    double pct = (t >= 0.0 && t <= 1.0) ? t * 100.0 : t;
    if (pct <= 0.0)
        return 0;
    if (pct >= 100.0)
        return 100;
    return (int64_t)(pct + 0.5);
}

/// @brief Sample the gradient at a normalized position and return raw RGBA.
/// @param gradient Borrowed Gradient2D handle.
/// @param t Normalized position or legacy percentage.
/// @return The sampled raw `0xRRGGBBAA` color.
int64_t rt_gradient2d_sample_normalized(void *gradient, double t) {
    return rt_gradient2d_sample(gradient, gradient2d_position_to_percent(t));
}

/// @brief Sample the gradient at a normalized position and return a Color-compatible value.
/// @param gradient Borrowed Gradient2D handle.
/// @param t Normalized position or legacy percentage.
/// @return The sample converted to tagged Color representation.
int64_t rt_gradient2d_sample_color_normalized(void *gradient, double t) {
    return rt_pixels_rgba_to_color((uint32_t)rt_gradient2d_sample_normalized(gradient, t));
}

/// @brief Fill `pixels` with a left-to-right horizontal gradient.
/// @details Maps each column X to a sample position `X * 100 / (width - 1)` and writes
///          the same sampled color to all rows in that column.
/// @param gradient Borrowed Gradient2D supplying colors.
/// @param pixels Borrowed destination Pixels image modified in place.
void rt_gradient2d_fill_horizontal(void *gradient, void *pixels) {
    rt_gradient2d_impl *impl =
        rt2d_has_class(gradient, RT2D_GRADIENT_CLASS_ID) ? (rt_gradient2d_impl *)gradient : NULL;
    if (!impl || !pixels)
        return;
    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    if (width <= 0 || height <= 0)
        return;
    int64_t denom = width > 1 ? width - 1 : 1;
    /* The sample is constant down each column, so compute one color per column
     * (width samples) instead of resampling the gradient for every pixel
     * (width*height) — mirrors the vertical variant's per-row caching. */
    uint32_t *cols = ((uint64_t)width <= SIZE_MAX / sizeof(uint32_t))
                         ? (uint32_t *)malloc((size_t)width * sizeof(uint32_t))
                         : NULL;
    if (!cols) {
        for (int64_t y = 0; y < height; y++)
            for (int64_t x = 0; x < width; x++)
                rt_pixels_set(pixels, x, y, rt_gradient2d_sample(gradient, x * 100 / denom));
        return;
    }
    for (int64_t x = 0; x < width; x++)
        cols[x] = (uint32_t)rt_gradient2d_sample(gradient, x * 100 / denom);
    for (int64_t y = 0; y < height; y++)
        for (int64_t x = 0; x < width; x++)
            rt_pixels_set(pixels, x, y, cols[x]);
    free(cols);
}

/// @brief Fill `pixels` with a top-to-bottom vertical gradient.
/// @details Maps each row Y to a sample position `Y * 100 / (height - 1)` and writes
///          the full row with a single color — avoids redundant samples per pixel.
/// @param gradient Borrowed Gradient2D supplying colors.
/// @param pixels Borrowed destination Pixels image modified in place.
void rt_gradient2d_fill_vertical(void *gradient, void *pixels) {
    rt_gradient2d_impl *impl =
        rt2d_has_class(gradient, RT2D_GRADIENT_CLASS_ID) ? (rt_gradient2d_impl *)gradient : NULL;
    if (!impl || !pixels)
        return;
    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    int64_t denom = height > 1 ? height - 1 : 1;
    for (int64_t y = 0; y < height; y++) {
        uint32_t color = (uint32_t)rt_gradient2d_sample(gradient, y * 100 / denom);
        for (int64_t x = 0; x < width; x++)
            rt_pixels_set(pixels, x, y, color);
    }
}

/// @brief GC finalizer — releases the retained Camera2D reference.
/// @param obj CameraRig2D payload being finalized.
static void camerarig2d_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_CAMERARIG_CLASS_ID))
        return;
    rt_camerarig2d_impl *impl = (rt_camerarig2d_impl *)obj;
    rt2d_release_ref_slot(&impl->camera);
}

/// @brief Allocate a CameraRig2D wrapping `camera` with default smoothing (100%).
/// @details `smoothing` of 100 means instant snapping (camera jumps straight to target);
///          lower values produce lag-based smooth follow. Shake starts zeroed.
/// @param camera Borrowed valid Camera2D to retain, or `NULL`.
/// @return A new owned CameraRig2D, or `NULL` for an invalid camera or allocation failure.
void *rt_camerarig2d_new(void *camera) {
    if (camera && !rt_obj_is_instance(camera, RT_CAMERA_CLASS_ID, 0))
        return NULL;
    rt2d_retain_ref(camera);
    rt_camerarig2d_impl *impl = (rt_camerarig2d_impl *)rt_obj_new_i64(
        RT2D_CAMERARIG_CLASS_ID, (int64_t)sizeof(rt_camerarig2d_impl));
    if (!impl) {
        void *owned_camera = camera;
        rt2d_release_ref_slot(&owned_camera);
        return NULL;
    }
    impl->camera = camera;
    impl->target_x = 0;
    impl->target_y = 0;
    impl->smoothing = 100;
    impl->shake_x = 0;
    impl->shake_y = 0;
    rt_obj_set_finalizer(impl, camerarig2d_finalize);
    return impl;
}

/// @brief Replace the Camera2D, retaining a reference to the new one.
/// @param rig Borrowed CameraRig2D handle to update.
/// @param camera Borrowed valid Camera2D to retain, or `NULL`.
void rt_camerarig2d_set_camera(void *rig, void *camera) {
    if (rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID) &&
        (!camera || rt_obj_is_instance(camera, RT_CAMERA_CLASS_ID, 0)))
        spriterenderer2d_set_ref(&((rt_camerarig2d_impl *)rig)->camera, camera);
}

/// @brief Set the world-space target position the camera should track.
/// @param rig Borrowed CameraRig2D handle to update.
/// @param x Target X coordinate.
/// @param y Target Y coordinate.
void rt_camerarig2d_set_target(void *rig, int64_t x, int64_t y) {
    if (!rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID))
        return;
    rt_camerarig2d_impl *impl = (rt_camerarig2d_impl *)rig;
    impl->target_x = x;
    impl->target_y = y;
}

/// @brief Set the smooth-follow strength in percent [0, 100].
/// @details 100 = snap instantly, 0 = camera never moves, ~5 = gentle follow.
/// @param rig Borrowed CameraRig2D handle to update.
/// @param lerp_pct Requested per-update strength.
void rt_camerarig2d_set_smoothing(void *rig, int64_t lerp_pct) {
    if (rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID))
        ((rt_camerarig2d_impl *)rig)->smoothing = clamp_i64(lerp_pct, 0, 100);
}

/// @brief Set the camera's deadzone rectangle (camera doesn't move while target is inside).
/// @param rig Borrowed CameraRig2D whose camera is configured.
/// @param width Dead-zone width passed to Camera2D.
/// @param height Dead-zone height passed to Camera2D.
void rt_camerarig2d_set_deadzone(void *rig, int64_t width, int64_t height) {
    rt_camerarig2d_impl *impl =
        rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID) ? (rt_camerarig2d_impl *)rig : NULL;
    if (impl && impl->camera)
        rt_camera_set_deadzone(impl->camera, width, height);
}

/// @brief Add a shake displacement offset (accumulated via saturating addition).
/// @param rig Borrowed CameraRig2D handle to update.
/// @param x X displacement to add.
/// @param y Y displacement to add.
void rt_camerarig2d_add_shake(void *rig, int64_t x, int64_t y) {
    if (!rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID))
        return;
    rt_camerarig2d_impl *impl = (rt_camerarig2d_impl *)rig;
    impl->shake_x = rt2d_saturating_add_i64(impl->shake_x, x);
    impl->shake_y = rt2d_saturating_add_i64(impl->shake_y, y);
}

/// @brief Reset the shake offset to zero.
/// @param rig Borrowed CameraRig2D handle to update.
void rt_camerarig2d_clear_shake(void *rig) {
    if (!rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID))
        return;
    rt_camerarig2d_impl *impl = (rt_camerarig2d_impl *)rig;
    impl->shake_x = 0;
    impl->shake_y = 0;
}

/// @brief Advance the rig by calling `rt_camera_smooth_follow` toward the current target.
/// @details Should be called once per frame before querying render positions.
/// @param rig Borrowed CameraRig2D handle to update.
void rt_camerarig2d_update(void *rig) {
    rt_camerarig2d_impl *impl =
        rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID) ? (rt_camerarig2d_impl *)rig : NULL;
    if (impl && impl->camera)
        rt_camera_smooth_follow(
            impl->camera, impl->target_x, impl->target_y, clamp_i64(impl->smoothing, 0, 100) * 10);
}

/// @brief Return the X render position: camera X plus current shake X offset.
/// @param rig Borrowed CameraRig2D handle.
/// @return The saturating render X coordinate, or `0` if invalid or disconnected.
int64_t rt_camerarig2d_get_render_x(void *rig) {
    rt_camerarig2d_impl *impl =
        rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID) ? (rt_camerarig2d_impl *)rig : NULL;
    return impl && impl->camera ? rt2d_saturating_add_i64(rt_camera_get_x(impl->camera), impl->shake_x)
                                : 0;
}

/// @brief Return the Y render position: camera Y plus current shake Y offset.
/// @param rig Borrowed CameraRig2D handle.
/// @return The saturating render Y coordinate, or `0` if invalid or disconnected.
int64_t rt_camerarig2d_get_render_y(void *rig) {
    rt_camerarig2d_impl *impl =
        rt2d_has_class(rig, RT2D_CAMERARIG_CLASS_ID) ? (rt_camerarig2d_impl *)rig : NULL;
    return impl && impl->camera ? rt2d_saturating_add_i64(rt_camera_get_y(impl->camera), impl->shake_y)
                                : 0;
}

/// @brief GC finalizer — releases the retained TexAtlas reference.
/// @param obj TexturePackerAtlas payload being finalized.
static void texturepackeratlas_finalize(void *obj) {
    if (!rt2d_has_class(obj, RT2D_TEXTUREPACKERATLAS_CLASS_ID))
        return;
    rt_texturepackeratlas_impl *impl = (rt_texturepackeratlas_impl *)obj;
    rt2d_release_ref_slot(&impl->atlas);
}

/// @brief Wrap a Pixels image as a TexturePackerAtlas, constructing a TexAtlas from it.
/// @details Traps with a message if `pixels` is NULL. The internal `rt_texatlas_new`
///          call may still return NULL on allocation failure, in which case this
///          function tears down the partially-constructed object cleanly.
/// @param pixels Borrowed source Pixels passed to TexAtlas.
/// @return A new owned TexturePackerAtlas, or `NULL` on construction failure.
void *rt_texturepackeratlas_new(void *pixels) {
    if (!pixels) {
        rt_trap("TexturePackerAtlas.New: null pixels");
        return NULL;
    }
    void *atlas = rt_texatlas_new(pixels);
    if (!atlas)
        return NULL;
    rt_texturepackeratlas_impl *impl = (rt_texturepackeratlas_impl *)rt_obj_new_i64(
        RT2D_TEXTUREPACKERATLAS_CLASS_ID, (int64_t)sizeof(rt_texturepackeratlas_impl));
    if (!impl) {
        rt2d_release_ref_slot(&atlas);
        return NULL;
    }
    impl->atlas = atlas;
    rt_obj_set_finalizer(impl, texturepackeratlas_finalize);
    return impl;
}

/// @brief Return the underlying TexAtlas pointer (not retained — caller must not release it).
/// @param packer Borrowed TexturePackerAtlas handle.
/// @return The borrowed TexAtlas, or `NULL` for an invalid handle.
void *rt_texturepackeratlas_get_atlas(void *packer) {
    return rt2d_has_class(packer, RT2D_TEXTUREPACKERATLAS_CLASS_ID)
               ? ((rt_texturepackeratlas_impl *)packer)->atlas
               : NULL;
}

/// @brief Register a named sub-region `(x, y, width, height)` in the atlas.
/// @param packer Borrowed TexturePackerAtlas handle to update.
/// @param name Borrowed region name.
/// @param x Region X coordinate.
/// @param y Region Y coordinate.
/// @param width Region width.
/// @param height Region height.
void rt_texturepackeratlas_add(
    void *packer, rt_string name, int64_t x, int64_t y, int64_t width, int64_t height) {
    void *atlas = rt_texturepackeratlas_get_atlas(packer);
    if (atlas)
        rt_texatlas_add(atlas, name, x, y, width, height);
}

/// @brief Return non-zero if a region with the given `name` exists in the atlas.
/// @param packer Borrowed TexturePackerAtlas handle.
/// @param name Borrowed region name to query.
/// @return The underlying atlas membership result, or `0` if invalid.
int64_t rt_texturepackeratlas_has(void *packer, rt_string name) {
    void *atlas = rt_texturepackeratlas_get_atlas(packer);
    return atlas ? rt_texatlas_has(atlas, name) : 0;
}

/// @brief Return the total number of named regions registered in the atlas.
/// @param packer Borrowed TexturePackerAtlas handle.
/// @return The registered region count, or `0` for an invalid handle.
int64_t rt_texturepackeratlas_region_count(void *packer) {
    void *atlas = rt_texturepackeratlas_get_atlas(packer);
    return atlas ? rt_texatlas_region_count(atlas) : 0;
}

/// @brief Allocate an AsepriteImporter with zeroed grid dimensions (must call SetGrid first).
/// @return A new owned AsepriteImporter, or `NULL` on allocation failure.
void *rt_asepriteimporter_new(void) {
    rt_asepriteimporter_impl *impl = (rt_asepriteimporter_impl *)rt_obj_new_i64(
        RT2D_ASEPRITEIMPORTER_CLASS_ID, (int64_t)sizeof(rt_asepriteimporter_impl));
    if (!impl)
        return NULL;
    return impl;
}

/// @brief Set the frame cell dimensions for slicing a uniform-grid sprite sheet.
/// @details Positive dimensions are sanitized via `normalized_dim`; zero or negative
///          dimensions leave the grid unset so ToAtlas fails cleanly.
/// @param importer Borrowed AsepriteImporter handle to update.
/// @param frame_width Requested frame width.
/// @param frame_height Requested frame height.
void rt_asepriteimporter_set_grid(void *importer, int64_t frame_width, int64_t frame_height) {
    if (!rt2d_has_class(importer, RT2D_ASEPRITEIMPORTER_CLASS_ID))
        return;
    rt_asepriteimporter_impl *impl = (rt_asepriteimporter_impl *)importer;
    impl->frame_width = frame_width > 0 ? normalized_dim(frame_width) : 0;
    impl->frame_height = frame_height > 0 ? normalized_dim(frame_height) : 0;
}

/// @brief Return the configured frame width in pixels (0 if not set).
/// @param importer Borrowed AsepriteImporter handle.
/// @return The frame width, or `0` when unset or invalid.
int64_t rt_asepriteimporter_get_frame_width(void *importer) {
    return rt2d_has_class(importer, RT2D_ASEPRITEIMPORTER_CLASS_ID)
               ? ((rt_asepriteimporter_impl *)importer)->frame_width
               : 0;
}

/// @brief Return the configured frame height in pixels (0 if not set).
/// @param importer Borrowed AsepriteImporter handle.
/// @return The frame height, or `0` when unset or invalid.
int64_t rt_asepriteimporter_get_frame_height(void *importer) {
    return rt2d_has_class(importer, RT2D_ASEPRITEIMPORTER_CLASS_ID)
               ? ((rt_asepriteimporter_impl *)importer)->frame_height
               : 0;
}

/// @brief Slice `pixels` into a grid atlas using the configured frame dimensions.
/// @details Returns NULL if any required value is missing or zero. Delegates to
///          `rt_texatlas_load_grid`; the returned atlas owns its region data.
/// @param importer Borrowed configured AsepriteImporter.
/// @param pixels Borrowed source Pixels image.
/// @return A new owned TexAtlas, or `NULL` for invalid input or construction failure.
void *rt_asepriteimporter_to_atlas(void *importer, void *pixels) {
    rt_asepriteimporter_impl *impl = rt2d_has_class(importer, RT2D_ASEPRITEIMPORTER_CLASS_ID)
                                         ? (rt_asepriteimporter_impl *)importer
                                         : NULL;
    if (!impl || !pixels || impl->frame_width <= 0 || impl->frame_height <= 0)
        return NULL;
    return rt_texatlas_load_grid(pixels, impl->frame_width, impl->frame_height);
}

/// @brief Allocate a TiledMapLoader with default tile size 16×16.
/// @return A new owned TiledMapLoader, or `NULL` on allocation failure.
void *rt_tiledmaploader_new(void) {
    rt_tiledmaploader_impl *impl = (rt_tiledmaploader_impl *)rt_obj_new_i64(
        RT2D_TILEDMAPLOADER_CLASS_ID, (int64_t)sizeof(rt_tiledmaploader_impl));
    if (!impl)
        return NULL;
    impl->tile_width = 16;
    impl->tile_height = 16;
    return impl;
}

/// @brief Override the tile dimensions used when creating new tilemaps.
/// @details Dimensions are sanitized via `normalized_dim` (non-positive → 1).
/// @param loader Borrowed TiledMapLoader handle to update.
/// @param tile_width Requested tile width.
/// @param tile_height Requested tile height.
void rt_tiledmaploader_set_tile_size(void *loader, int64_t tile_width, int64_t tile_height) {
    if (!rt2d_has_class(loader, RT2D_TILEDMAPLOADER_CLASS_ID))
        return;
    rt_tiledmaploader_impl *impl = (rt_tiledmaploader_impl *)loader;
    impl->tile_width = normalized_dim(tile_width);
    impl->tile_height = normalized_dim(tile_height);
}

/// @brief Return the configured tile width in pixels.
/// @param loader Borrowed TiledMapLoader handle.
/// @return The tile width, or `0` for an invalid handle.
int64_t rt_tiledmaploader_get_tile_width(void *loader) {
    return rt2d_has_class(loader, RT2D_TILEDMAPLOADER_CLASS_ID)
               ? ((rt_tiledmaploader_impl *)loader)->tile_width
               : 0;
}

/// @brief Return the configured tile height in pixels.
/// @param loader Borrowed TiledMapLoader handle.
/// @return The tile height, or `0` for an invalid handle.
int64_t rt_tiledmaploader_get_tile_height(void *loader) {
    return rt2d_has_class(loader, RT2D_TILEDMAPLOADER_CLASS_ID)
               ? ((rt_tiledmaploader_impl *)loader)->tile_height
               : 0;
}

/// @brief Allocate a blank Tilemap2D with `width × height` cells using this loader's tile size.
/// @details Delegates to `rt_tilemap_new` — the returned tilemap has all cells set to 0 (empty).
/// @param loader Borrowed TiledMapLoader supplying tile dimensions.
/// @param width Requested map width in cells.
/// @param height Requested map height in cells.
/// @return A new owned Tilemap, or `NULL` for an invalid loader or allocation failure.
void *rt_tiledmaploader_new_tilemap(void *loader, int64_t width, int64_t height) {
    rt_tiledmaploader_impl *impl = rt2d_has_class(loader, RT2D_TILEDMAPLOADER_CLASS_ID)
                                       ? (rt_tiledmaploader_impl *)loader
                                       : NULL;
    if (!impl)
        return NULL;
    return rt_tilemap_new(width, height, impl->tile_width, impl->tile_height);
}
