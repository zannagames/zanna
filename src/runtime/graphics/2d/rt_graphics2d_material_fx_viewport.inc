//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics2d_material_fx_viewport.inc
/// @brief Material2D, Shader2D, PostProcess2D, Viewport2D, and ScreenScaler
///        runtime entry points.
///
/// @details This include fragment is consumed only by rt_graphics2d.c and
/// groups the stateful rendering configuration and scene-view helpers that sit
/// above the lower-level surface and texture primitives. It provides immutable
/// pixel-effect results rather than mutating source images, and it centralizes
/// the fixed-point coordinate mapping shared by Viewport2D and ScreenScaler.
///
// File: src/runtime/graphics/2d/rt_graphics2d_material_fx_viewport.inc
// Purpose: Material2D, Shader2D, PostProcess2D, Viewport2D, and ScreenScaler entry points.
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
// Material2D
//=============================================================================
// Stateless tint / alpha / blend-mode container. Unlike Renderer2D it doesn't
// queue draws — `Apply(pixels)` produces a tint+alpha-processed copy of the
// input Pixels and callers feed that to whichever draw surface they use.

/// @brief Allocate a Material2D with no tint, full alpha, and alpha blending.
/// @return A new Material2D handle, or `NULL` if the runtime object allocation
///         fails.
void *rt_material2d_new(void) {
    rt_material2d_impl *material = (rt_material2d_impl *)rt_obj_new_i64(
        RT2D_MATERIAL_CLASS_ID, (int64_t)sizeof(rt_material2d_impl));
    if (!material)
        return NULL;
    material->tint = -1;
    material->alpha = 255;
    material->blend_mode = RT_GRAPHICS2D_BLEND_ALPHA;
    return material;
}

/// @brief Set the material's multiplicative tint color.
/// @param material Opaque Material2D handle to update; invalid handles are
///                 ignored.
/// @param rgb Tint encoded as `0x00RRGGBB`; a negative value disables tinting,
///            while nonnegative values are masked to 24 bits.
void rt_material2d_set_tint(void *material, int64_t rgb) {
    if (rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID))
        ((rt_material2d_impl *)material)->tint = rgb < 0 ? -1 : (rgb & 0x00FFFFFF);
}

/// @brief Get the material's current multiplicative tint color.
/// @param material Opaque Material2D handle to query.
/// @return The tint encoded as `0x00RRGGBB`, or `-1` when tinting is disabled
///         or @p material is invalid.
int64_t rt_material2d_get_tint(void *material) {
    return rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID) ? ((rt_material2d_impl *)material)->tint
                                                            : -1;
}

/// @brief Set the alpha scale clamped to `[0, 255]`.
/// @param material Opaque Material2D handle to update; invalid handles are
///                 ignored.
/// @param alpha Requested alpha multiplier, where `0` is transparent and `255`
///              preserves the source alpha.
void rt_material2d_set_alpha(void *material, int64_t alpha) {
    if (rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID))
        ((rt_material2d_impl *)material)->alpha = clamp_u8_i64(alpha);
}

/// @brief Get the current alpha scale.
/// @param material Opaque Material2D handle to query.
/// @return The stored alpha multiplier in `[0, 255]`, or `255` when
///         @p material is invalid.
int64_t rt_material2d_get_alpha(void *material) {
    return rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID)
               ? ((rt_material2d_impl *)material)->alpha
               : 255;
}

/// @brief Set the material's compositing blend mode.
/// @param material Opaque Material2D handle to update; invalid handles are
///                 ignored.
/// @param blend_mode Requested `RT_GRAPHICS2D_BLEND_*` value, clamped to the
///                   supported numeric range `[0, 2]`.
void rt_material2d_set_blend_mode(void *material, int64_t blend_mode) {
    if (rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID))
        ((rt_material2d_impl *)material)->blend_mode = clamp_i64(blend_mode, 0, 2);
}

/// @brief Get the material's current compositing blend mode.
/// @param material Opaque Material2D handle to query.
/// @return The stored `RT_GRAPHICS2D_BLEND_*` value, or
///         `RT_GRAPHICS2D_BLEND_ALPHA` when @p material is invalid.
int64_t rt_material2d_get_blend_mode(void *material) {
    return rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID)
               ? ((rt_material2d_impl *)material)->blend_mode
               : RT_GRAPHICS2D_BLEND_ALPHA;
}

/// @brief Produce a tint+alpha-applied copy of `pixels` using this material's state.
/// @details Clones the source and applies tint / alpha in place on the clone;
///          the original Pixels is untouched. Returns NULL if either argument is
///          NULL or the clone fails. Blend mode is NOT baked in — it's a
///          *compositing* choice made when the processed Pixels are drawn, not
///          when they're produced.
/// @param material Opaque Material2D handle whose tint and alpha are applied.
/// @param pixels Source Pixels handle; its pixel buffer is never modified.
/// @return A newly allocated Pixels object containing the processed copy, or
///         `NULL` for invalid input or allocation failure.
void *rt_material2d_apply(void *material, void *pixels) {
    if (!rt2d_has_class(material, RT2D_MATERIAL_CLASS_ID) || !pixels)
        return NULL;
    rt_material2d_impl *impl = (rt_material2d_impl *)material;
    void *out = rt_pixels_clone(pixels);
    if (out)
        apply_tint_alpha_in_place(out, impl->tint, impl->alpha);
    return out;
}

//=============================================================================
// Shader2D / PostProcess2D
//=============================================================================
// CPU image-effect wrappers. Five effects: NONE (passthrough clone), INVERT,
// GRAYSCALE, TINT (colorize toward `color`), BLUR (box blur with radius derived
// from `amount`). Shader2D and PostProcess2D have distinct runtime class
// identities but compatible state layouts and use the same effect dispatcher.
// The two classes let user code distinguish a shader-like image transform from
// a full-screen post-processing pass without duplicating the pixel algorithms.

/// @brief Allocate a Shader2D with the requested initial effect.
/// @details Effect index is clamped to the valid range `[0, 4]`; defaults to
///          `amount = 1` (blur radius / tint intensity) and white `color`.
/// @param effect Requested `RT_GRAPHICS2D_EFFECT_*` value.
/// @return A new Shader2D handle, or `NULL` if allocation fails.
void *rt_shader2d_new(int64_t effect) {
    rt_shader2d_impl *shader =
        (rt_shader2d_impl *)rt_obj_new_i64(RT2D_SHADER_CLASS_ID, (int64_t)sizeof(rt_shader2d_impl));
    if (!shader)
        return NULL;
    shader->effect = clamp_i64(effect, 0, 4);
    shader->amount = 1;
    shader->color = 0x00FFFFFF;
    return shader;
}

/// @brief Set the shader effect index clamped to `[0, 4]` (NONE/INVERT/GRAYSCALE/TINT/BLUR).
/// @param shader Opaque Shader2D handle to update; invalid handles are ignored.
/// @param effect Requested `RT_GRAPHICS2D_EFFECT_*` value.
void rt_shader2d_set_effect(void *shader, int64_t effect) {
    rt_shader2d_impl *impl = shader2d_checked(shader);
    if (impl)
        impl->effect = clamp_i64(effect, 0, 4);
}

/// @brief Get the current effect index, defaulting to NONE for invalid handles.
/// @param shader Opaque Shader2D handle to query.
/// @return The stored `RT_GRAPHICS2D_EFFECT_*` value, or
///         `RT_GRAPHICS2D_EFFECT_NONE` when @p shader is invalid.
int64_t rt_shader2d_get_effect(void *shader) {
    rt_shader2d_impl *impl = shader2d_checked(shader);
    return impl ? impl->effect : RT_GRAPHICS2D_EFFECT_NONE;
}

/// @brief Set the effect amount (e.g. blur radius) clamped to `[0, 10]`.
/// @param shader Opaque Shader2D handle to update; invalid handles are ignored.
/// @param amount Requested effect-specific amount.
void rt_shader2d_set_amount(void *shader, int64_t amount) {
    rt_shader2d_impl *impl = shader2d_checked(shader);
    if (impl)
        impl->amount = clamp_i64(amount, 0, 10);
}

/// @brief Get the current effect amount, or 0 for invalid handles.
/// @param shader Opaque Shader2D handle to query.
/// @return The stored amount in `[0, 10]`, or `0` when @p shader is invalid.
int64_t rt_shader2d_get_amount(void *shader) {
    rt_shader2d_impl *impl = shader2d_checked(shader);
    return impl ? impl->amount : 0;
}

/// @brief Set the effect color (`0x00RRGGBB`), used for the TINT effect.
/// @param shader Opaque Shader2D handle to update; invalid handles are ignored.
/// @param rgb Effect color whose low 24 bits are stored.
void rt_shader2d_set_color(void *shader, int64_t rgb) {
    rt_shader2d_impl *impl = shader2d_checked(shader);
    if (impl)
        impl->color = rgb & 0x00FFFFFF;
}

/// @brief Get the current effect color, defaulting to white (0x00FFFFFF) for invalid handles.
/// @param shader Opaque Shader2D handle to query.
/// @return The stored color encoded as `0x00RRGGBB`, or white when @p shader is
///         invalid.
int64_t rt_shader2d_get_color(void *shader) {
    rt_shader2d_impl *impl = shader2d_checked(shader);
    return impl ? impl->color : 0x00FFFFFF;
}

/// @brief Dispatch to the correct pixel-effect function for the given effect index.
/// @details Returns a new Pixels buffer with the effect applied. Effect NONE returns a plain clone.
///   INVERT, GRAYSCALE, TINT, and BLUR delegate to the corresponding `rt_pixels_*` functions.
/// @param effect `RT_GRAPHICS2D_EFFECT_*` operation to dispatch; unrecognized
///               values use the NONE behavior.
/// @param amount Effect-specific amount passed to blur and otherwise ignored.
/// @param color Effect color passed to tint and otherwise ignored.
/// @param pixels Source Pixels handle to process.
/// @return A newly allocated Pixels result, or `NULL` when @p pixels is `NULL`
///         or the selected pixel operation fails.
static void *apply_effect(int64_t effect, int64_t amount, int64_t color, void *pixels) {
    if (!pixels)
        return NULL;
    switch (effect) {
        case RT_GRAPHICS2D_EFFECT_INVERT:
            return rt_pixels_invert(pixels);
        case RT_GRAPHICS2D_EFFECT_GRAYSCALE:
            return rt_pixels_grayscale(pixels);
        case RT_GRAPHICS2D_EFFECT_TINT:
            return rt_pixels_tint(pixels, color);
        case RT_GRAPHICS2D_EFFECT_BLUR:
            return rt_pixels_blur(pixels, amount);
        case RT_GRAPHICS2D_EFFECT_NONE:
        default:
            return rt_pixels_clone(pixels);
    }
}

/// @brief Apply the shader's effect to @p pixels and return a new Pixels buffer with the result.
/// @details A NULL shader degrades gracefully to a plain clone. Callers own the returned Pixels.
/// @param shader Opaque Shader2D handle, or `NULL` to request a plain clone.
/// @param pixels Source Pixels handle; it is not modified.
/// @return A newly allocated Pixels result, or `NULL` for invalid non-null
///         handles, null pixels, or an allocation/effect failure.
void *rt_shader2d_apply(void *shader, void *pixels) {
    if (!shader)
        return pixels ? rt_pixels_clone(pixels) : NULL;
    if (!shader2d_checked(shader))
        return NULL;
    rt_shader2d_impl *impl = (rt_shader2d_impl *)shader;
    return apply_effect(impl->effect, impl->amount, impl->color, pixels);
}

/// @brief Allocate a PostProcess2D (a Shader2D starting with EFFECT_NONE).
/// @return A new PostProcess2D handle with amount `1` and white effect color,
///         or `NULL` if allocation fails.
void *rt_postprocess2d_new(void) {
    rt_postprocess2d_impl *postprocess = (rt_postprocess2d_impl *)rt_obj_new_i64(
        RT2D_POSTPROCESS_CLASS_ID, (int64_t)sizeof(rt_postprocess2d_impl));
    if (!postprocess)
        return NULL;
    postprocess->effect = RT_GRAPHICS2D_EFFECT_NONE;
    postprocess->amount = 1;
    postprocess->color = 0x00FFFFFF;
    return postprocess;
}

/// @brief Set the post-process effect index.
/// @param postprocess Opaque PostProcess2D handle to update; invalid handles
///                    are ignored.
/// @param effect Requested `RT_GRAPHICS2D_EFFECT_*` value, clamped to `[0, 4]`.
void rt_postprocess2d_set_effect(void *postprocess, int64_t effect) {
    rt_postprocess2d_impl *impl = postprocess2d_checked(postprocess);
    if (impl)
        impl->effect = clamp_i64(effect, 0, 4);
}

/// @brief Set the post-process effect amount.
/// @param postprocess Opaque PostProcess2D handle to update; invalid handles
///                    are ignored.
/// @param amount Requested effect-specific amount, clamped to `[0, 10]`.
void rt_postprocess2d_set_amount(void *postprocess, int64_t amount) {
    rt_postprocess2d_impl *impl = postprocess2d_checked(postprocess);
    if (impl)
        impl->amount = clamp_i64(amount, 0, 10);
}

/// @brief Set the post-process effect color.
/// @param postprocess Opaque PostProcess2D handle to update; invalid handles
///                    are ignored.
/// @param rgb Effect color whose low 24 bits are stored as `0x00RRGGBB`.
void rt_postprocess2d_set_color(void *postprocess, int64_t rgb) {
    rt_postprocess2d_impl *impl = postprocess2d_checked(postprocess);
    if (impl)
        impl->color = rgb & 0x00FFFFFF;
}

/// @brief Apply the configured post-process effect to a pixel image.
/// @details A null post-process handle requests a plain clone, while an invalid
///          non-null handle is rejected. The source image is never modified.
/// @param postprocess Opaque PostProcess2D handle, or `NULL` for no effect.
/// @param pixels Source Pixels handle to process.
/// @return A newly allocated Pixels result, or `NULL` for invalid input or an
///         allocation/effect failure.
void *rt_postprocess2d_apply(void *postprocess, void *pixels) {
    if (!postprocess)
        return pixels ? rt_pixels_clone(pixels) : NULL;
    rt_postprocess2d_impl *impl = postprocess2d_checked(postprocess);
    if (!impl)
        return NULL;
    return apply_effect(impl->effect, impl->amount, impl->color, pixels);
}

//=============================================================================
// Viewport2D (aliased as ScreenScaler)
//=============================================================================
// Maps a fixed *virtual* resolution (the author's design size) onto the
// actual *screen* resolution with letterboxing. Scale is fixed-point with
// `1000 == 1.0x`. Consumers apply `Scale` + `OffsetX` / `OffsetY` to translate
// between world coordinates and screen coordinates. The class's superpower is
// *integer scaling* — when the screen can display the virtual canvas at 1x or
// larger, retro pixel games can select `1.0x`, `2.0x`, `3.0x`, and so on
// instead of fractional enlargement that produces uneven texel sizes.

/// @brief Recompute `scale`, `offset_x`, `offset_y` from the four size fields.
/// @details Picks the largest uniform scale that fits the virtual canvas on
///          both axes: the smaller of the independent X and Y scale factors.
///          When integer scaling is enabled and the fit is at least `1.0x`,
///          the scale is truncated to a whole multiple of `1000`. Fractional
///          scales below `1.0x` remain available so oversized virtual canvases
///          can still be reduced to fit. Offsets center the scaled canvas and
///          split letterbox or pillarbox space between opposite sides.
///
///          Fixed-point representation: integer math with a 1000x multiplier
///          throughout, so `scale = 2300` means `2.3x`. All downstream
///          `world_to_screen` / `screen_to_world` calls use the same fixed-
///          point arithmetic so round-trips are stable within one-pixel
///          tolerance.
/// @param viewport Mutable viewport state whose normalized dimensions and
///                 integer-scaling flag are already initialized; `NULL` is
///                 accepted as a no-op.
static void viewport2d_recalculate(rt_viewport2d_impl *viewport) {
    if (!viewport)
        return;
    long double scale_x_ld =
        ((long double)viewport->screen_width * 1000.0L) / (long double)viewport->virtual_width;
    long double scale_y_ld =
        ((long double)viewport->screen_height * 1000.0L) / (long double)viewport->virtual_height;
    int64_t scale_x = scale_x_ld >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scale_x_ld;
    int64_t scale_y = scale_y_ld >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scale_y_ld;
    viewport->scale = scale_x < scale_y ? scale_x : scale_y;
    if (viewport->scale < 1)
        viewport->scale = 1;
    if (viewport->integer_scaling && viewport->scale >= 1000) {
        int64_t whole = viewport->scale / 1000;
        viewport->scale = whole > INT64_MAX / 1000 ? INT64_MAX : whole * 1000;
    }
    long double scaled_width_ld =
        ((long double)viewport->virtual_width * (long double)viewport->scale) / 1000.0L;
    long double scaled_height_ld =
        ((long double)viewport->virtual_height * (long double)viewport->scale) / 1000.0L;
    int64_t scaled_width =
        scaled_width_ld >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scaled_width_ld;
    int64_t scaled_height =
        scaled_height_ld >= (long double)INT64_MAX ? INT64_MAX : (int64_t)scaled_height_ld;
    viewport->offset_x = (viewport->screen_width - scaled_width) / 2;
    viewport->offset_y = (viewport->screen_height - scaled_height) / 2;
}

/// @brief Allocate and initialize viewport-compatible state for a runtime class.
/// @details All dimensions are normalized to `[1, RT2D_MAX_DIM]` before the
///          initial fixed-point scale and centered offsets are calculated.
/// @param virtual_width Requested width of the design-space canvas.
/// @param virtual_height Requested height of the design-space canvas.
/// @param screen_width Requested width of the physical output.
/// @param screen_height Requested height of the physical output.
/// @param class_id Runtime class identifier to assign to the new object.
/// @return A new initialized object, or `NULL` if runtime allocation fails.
static void *viewport2d_new_with_class(int64_t virtual_width,
                                       int64_t virtual_height,
                                       int64_t screen_width,
                                       int64_t screen_height,
                                       int64_t class_id) {
    rt_viewport2d_impl *viewport =
        (rt_viewport2d_impl *)rt_obj_new_i64(class_id, (int64_t)sizeof(rt_viewport2d_impl));
    if (!viewport)
        return NULL;
    viewport->virtual_width = normalized_dim(virtual_width);
    viewport->virtual_height = normalized_dim(virtual_height);
    viewport->screen_width = normalized_dim(screen_width);
    viewport->screen_height = normalized_dim(screen_height);
    viewport2d_recalculate(viewport);
    return viewport;
}

/// @brief Allocate a Viewport2D with the given virtual and screen dimensions.
/// @details Dimensions are all clamped to `[1, RT2D_MAX_DIM]` by `normalized_dim`
///          so zero / negative inputs don't produce divide-by-zero in the scale
///          computation. Integer scaling is disabled by default. Scale + offsets
///          are computed immediately so the viewport is usable without a
///          separate `Recalculate` call.
/// @param virtual_width Requested width of the design-space canvas.
/// @param virtual_height Requested height of the design-space canvas.
/// @param screen_width Requested width of the physical output.
/// @param screen_height Requested height of the physical output.
/// @return A new Viewport2D handle, or `NULL` if allocation fails.
void *rt_viewport2d_new(int64_t virtual_width,
                        int64_t virtual_height,
                        int64_t screen_width,
                        int64_t screen_height) {
    return viewport2d_new_with_class(
        virtual_width, virtual_height, screen_width, screen_height, RT2D_VIEWPORT_CLASS_ID);
}

/// @brief Allocate a ScreenScaler with distinct runtime type identity and Viewport2D behavior.
/// @param virtual_width Requested width of the design-space canvas.
/// @param virtual_height Requested height of the design-space canvas.
/// @param screen_width Requested width of the physical output.
/// @param screen_height Requested height of the physical output.
/// @return A new ScreenScaler handle, or `NULL` if allocation fails.
void *rt_screenscaler_new(int64_t virtual_width,
                          int64_t virtual_height,
                          int64_t screen_width,
                          int64_t screen_height) {
    return viewport2d_new_with_class(
        virtual_width, virtual_height, screen_width, screen_height, RT2D_SCREENSCALER_CLASS_ID);
}

/// @brief Update the virtual (design) canvas size and recalculate scale/offsets.
/// @param viewport Opaque Viewport2D or ScreenScaler handle; invalid handles are
///                 ignored.
/// @param width Requested virtual width, normalized to `[1, RT2D_MAX_DIM]`.
/// @param height Requested virtual height, normalized to `[1, RT2D_MAX_DIM]`.
void rt_viewport2d_set_virtual_size(void *viewport, int64_t width, int64_t height) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return;
    impl->virtual_width = normalized_dim(width);
    impl->virtual_height = normalized_dim(height);
    viewport2d_recalculate(impl);
}

/// @brief Update the physical screen size and recalculate scale/offsets.
/// @param viewport Opaque Viewport2D or ScreenScaler handle; invalid handles are
///                 ignored.
/// @param width Requested screen width, normalized to `[1, RT2D_MAX_DIM]`.
/// @param height Requested screen height, normalized to `[1, RT2D_MAX_DIM]`.
void rt_viewport2d_set_screen_size(void *viewport, int64_t width, int64_t height) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return;
    impl->screen_width = normalized_dim(width);
    impl->screen_height = normalized_dim(height);
    viewport2d_recalculate(impl);
}

/// @brief Enable or disable whole-number enlargement when the canvas fits at 1x.
/// @param viewport Opaque Viewport2D or ScreenScaler handle; invalid handles are
///                 ignored.
/// @param enabled Nonzero to quantize scales of at least `1.0x` to whole
///                multiples, or zero to retain fractional scaling.
void rt_viewport2d_set_integer_scaling(void *viewport, int64_t enabled) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return;
    impl->integer_scaling = enabled != 0;
    viewport2d_recalculate(impl);
}

/// @brief Return the current scale factor as a fixed-point integer (1000 = 1.0x).
/// @param viewport Opaque Viewport2D or ScreenScaler handle to query.
/// @return The calculated positive fixed-point scale, or `1000` when
///         @p viewport is invalid.
int64_t rt_viewport2d_get_scale(void *viewport) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    return impl ? impl->scale : 1000;
}

/// @brief Return the horizontal letterbox/pillarbox offset in screen pixels.
/// @param viewport Opaque Viewport2D or ScreenScaler handle to query.
/// @return The horizontal offset, or `0` when @p viewport is invalid.
int64_t rt_viewport2d_get_offset_x(void *viewport) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    return impl ? impl->offset_x : 0;
}

/// @brief Return the vertical letterbox/pillarbox offset in screen pixels.
/// @param viewport Opaque Viewport2D or ScreenScaler handle to query.
/// @return The vertical offset, or `0` when @p viewport is invalid.
int64_t rt_viewport2d_get_offset_y(void *viewport) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    return impl ? impl->offset_y : 0;
}

/// @brief Convert a virtual-space X coordinate to a screen-space X pixel position.
/// @details Applies `screen_x = offset_x + (virtual_x * scale / 1000)` using `long double`
///   to preserve sub-pixel accuracy before rounding to the nearest integer.
/// @param viewport Opaque Viewport2D or ScreenScaler handle.
/// @param x Virtual-space X coordinate to map.
/// @return The nearest screen-space X coordinate, saturated to `int64_t`; if
///         @p viewport is invalid, returns @p x unchanged.
int64_t rt_viewport2d_world_to_screen_x(void *viewport, int64_t x) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return x;
    long double mapped =
        (long double)impl->offset_x + ((long double)x * (long double)impl->scale) / 1000.0L;
    return round_long_double_to_i64(mapped);
}

/// @brief Convert a virtual-space Y coordinate to a screen-space Y pixel position.
/// @param viewport Opaque Viewport2D or ScreenScaler handle.
/// @param y Virtual-space Y coordinate to map.
/// @return The nearest screen-space Y coordinate, saturated to `int64_t`; if
///         @p viewport is invalid, returns @p y unchanged.
int64_t rt_viewport2d_world_to_screen_y(void *viewport, int64_t y) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return y;
    long double mapped =
        (long double)impl->offset_y + ((long double)y * (long double)impl->scale) / 1000.0L;
    return round_long_double_to_i64(mapped);
}

/// @brief Convert a screen-space X pixel position back to a virtual-space X coordinate.
/// @details Inverse of `world_to_screen_x`: `virtual_x = (screen_x - offset_x) * 1000 / scale`.
/// @param viewport Opaque Viewport2D or ScreenScaler handle.
/// @param x Screen-space X coordinate to map.
/// @return The nearest virtual-space X coordinate, saturated to `int64_t`; if
///         @p viewport is invalid, returns @p x unchanged.
int64_t rt_viewport2d_screen_to_world_x(void *viewport, int64_t x) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return x;
    long double mapped =
        ((long double)x - (long double)impl->offset_x) * 1000.0L / (long double)impl->scale;
    return round_long_double_to_i64(mapped);
}

/// @brief Convert a screen-space Y pixel position back to a virtual-space Y coordinate.
/// @param viewport Opaque Viewport2D or ScreenScaler handle.
/// @param y Screen-space Y coordinate to map.
/// @return The nearest virtual-space Y coordinate, saturated to `int64_t`; if
///         @p viewport is invalid, returns @p y unchanged.
int64_t rt_viewport2d_screen_to_world_y(void *viewport, int64_t y) {
    rt_viewport2d_impl *impl = viewport2d_checked(viewport);
    if (!impl)
        return y;
    long double mapped =
        ((long double)y - (long double)impl->offset_y) * 1000.0L / (long double)impl->scale;
    return round_long_double_to_i64(mapped);
}
