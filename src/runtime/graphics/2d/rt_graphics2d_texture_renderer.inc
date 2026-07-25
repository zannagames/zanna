//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics2d_texture_renderer.inc
/// @brief Texture2D, GpuTexture2D, and Renderer2D runtime entry points.
///
/// @details This include fragment is consumed only by rt_graphics2d.c and
/// keeps texture ownership, sampling state, retained draw-command creation, and
/// target-specific command execution in one contiguous section of the split 2D
/// graphics implementation.
///
// File: src/runtime/graphics/2d/rt_graphics2d_texture_renderer.inc
// Purpose: Texture2D, GpuTexture2D, and Renderer2D command-stream entry points.
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
// Texture2D / GpuTexture2D
//=============================================================================
// Retained `Pixels` handle plus sampling state — filter mode (NEAREST / LINEAR)
// and wrap mode (CLAMP / REPEAT). The two classes have distinct runtime type
// identities but share a CPU-backed implementation in this module.

/// @brief GC finalizer — releases the retained Pixels reference.
/// @details Invalidates the implementation magic cookie before releasing the
///          backing image. Runtime object storage is reclaimed separately by
///          the object system.
/// @param obj Finalizing Texture2D or GpuTexture2D implementation supplied by
///            the runtime object system.
static void texture2d_finalize(void *obj) {
    rt_texture2d_impl *texture = (rt_texture2d_impl *)obj;
    texture->magic = 0;
    rt2d_release_ref_slot(&texture->pixels);
}

/// @brief Wrap an existing `Pixels` buffer as a Texture2D with default sampling
///        state (NEAREST filter, CLAMP wrap).
/// @details Takes a reference on `pixels`; the caller can drop theirs immediately.
///          Returns NULL on allocation failure; fires no trap so a caller probing
///          "was this pixels buffer loadable" can check for NULL without having
///          to catch a trap.
/// @param pixels Valid Pixels handle to retain as the texture backing image.
/// @param class_id Runtime class identifier for Texture2D or GpuTexture2D.
/// @return A new texture-compatible handle, or `NULL` for an invalid Pixels
///         handle or allocation failure.
static void *texture2d_new_with_class(void *pixels, int64_t class_id) {
    if (!pixels)
        return NULL;
    if (!rt_obj_is_instance(pixels, RT_PIXELS_CLASS_ID, sizeof(rt_pixels_impl)))
        return NULL;
    rt2d_retain_ref(pixels);
    rt_texture2d_impl *texture =
        (rt_texture2d_impl *)rt_obj_new_i64(class_id, (int64_t)sizeof(rt_texture2d_impl));
    if (!texture) {
        void *owned_pixels = pixels;
        rt2d_release_ref_slot(&owned_pixels);
        return NULL;
    }
    texture->magic = RT2D_TEXTURE_MAGIC;
    texture->pixels = pixels;
    texture->filter = RT_GRAPHICS2D_FILTER_NEAREST;
    texture->wrap = RT_GRAPHICS2D_WRAP_CLAMP;
    rt_obj_set_finalizer(texture, texture2d_finalize);
    return texture;
}

/// @brief Wrap an existing Pixels image as a CPU-backed Texture2D.
/// @details The texture retains its own reference and initializes NEAREST
///          filtering with CLAMP wrapping.
/// @param pixels Valid Pixels handle to retain.
/// @return A new Texture2D handle, or `NULL` for invalid input or allocation
///         failure.
void *rt_texture2d_new(void *pixels) {
    return texture2d_new_with_class(pixels, RT2D_TEXTURE_CLASS_ID);
}

/// @brief Wrap an existing Pixels image as a GpuTexture2D runtime object.
/// @details The current implementation remains CPU-backed but preserves a
///          distinct GpuTexture2D class identity. It retains @p pixels and uses
///          NEAREST filtering with CLAMP wrapping initially.
/// @param pixels Valid Pixels handle to retain.
/// @return A new GpuTexture2D handle, or `NULL` for invalid input or allocation
///         failure.
void *rt_gputexture2d_new(void *pixels) {
    return texture2d_new_with_class(pixels, RT2D_GPUTEXTURE_CLASS_ID);
}

/// @brief Load a Pixels buffer from disk and wrap it as a Texture2D.
/// @details The transient Pixels reference is released after the Texture2D takes its own
///   retain, leaving exactly the reference owned by the returned texture. Returns
///   NULL if the file cannot be loaded or the texture allocation fails.
/// @param path Runtime string naming the image file to load.
/// @param class_id Runtime class identifier for the wrapper object.
/// @return A new texture-compatible handle, or `NULL` if loading, validation,
///         or allocation fails.
static void *texture2d_from_file_with_class(rt_string path, int64_t class_id) {
    void *pixels = rt_pixels_load(path);
    if (!pixels)
        return NULL;
    void *texture = texture2d_new_with_class(pixels, class_id);
    rt2d_release_ref_slot(&pixels);
    return texture;
}

/// @brief Load an image file into a new Texture2D.
/// @param path Runtime string naming the image file.
/// @return A new Texture2D handle, or `NULL` when loading or allocation fails.
void *rt_texture2d_from_file(rt_string path) {
    return texture2d_from_file_with_class(path, RT2D_TEXTURE_CLASS_ID);
}

/// @brief Load an image file into a CPU-backed GpuTexture2D object.
/// @param path Runtime string naming the image file.
/// @return A new GpuTexture2D handle, or `NULL` when loading or allocation
///         fails.
void *rt_gputexture2d_from_file(rt_string path) {
    return texture2d_from_file_with_class(path, RT2D_GPUTEXTURE_CLASS_ID);
}

/// @brief Return the pixel width of the texture, or 0 for an invalid handle.
/// @param texture Opaque Texture2D or GpuTexture2D handle to query.
/// @return The backing Pixels width, or `0` when @p texture is invalid.
int64_t rt_texture2d_width(void *texture) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    return impl ? rt_pixels_width(impl->pixels) : 0;
}

/// @brief Return the pixel height of the texture, or 0 for an invalid handle.
/// @param texture Opaque Texture2D or GpuTexture2D handle to query.
/// @return The backing Pixels height, or `0` when @p texture is invalid.
int64_t rt_texture2d_height(void *texture) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    return impl ? rt_pixels_height(impl->pixels) : 0;
}

/// @brief Return the underlying Pixels buffer (borrowed reference — caller must not release).
/// @param texture Opaque Texture2D or GpuTexture2D handle to query.
/// @return The borrowed backing Pixels handle, or `NULL` when @p texture is
///         invalid. Its lifetime remains tied to the texture object.
void *rt_texture2d_get_pixels(void *texture) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    return impl ? impl->pixels : NULL;
}

/// @brief Clone the underlying Pixels buffer (caller owns the returned copy).
/// @param texture Opaque Texture2D or GpuTexture2D handle to copy.
/// @return A new independent Pixels handle, or `NULL` for an invalid texture or
///         allocation failure.
void *rt_texture2d_clone_pixels(void *texture) {
    void *pixels = rt_texture2d_get_pixels(texture);
    return pixels ? rt_pixels_clone(pixels) : NULL;
}

/// @brief Set the sampling filter mode (`RT_GRAPHICS2D_FILTER_NEAREST` or `LINEAR`).
/// @details Only the LINEAR constant selects linear sampling; every other value
///          normalizes to NEAREST.
/// @param texture Opaque Texture2D or GpuTexture2D handle to update; invalid
///                handles are ignored.
/// @param filter Requested `RT_GRAPHICS2D_FILTER_*` value.
void rt_texture2d_set_filter(void *texture, int64_t filter) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    if (!impl)
        return;
    impl->filter = (filter == RT_GRAPHICS2D_FILTER_LINEAR) ? RT_GRAPHICS2D_FILTER_LINEAR
                                                           : RT_GRAPHICS2D_FILTER_NEAREST;
}

/// @brief Get the current sampling filter mode, defaulting to NEAREST for invalid handles.
/// @param texture Opaque Texture2D or GpuTexture2D handle to query.
/// @return The stored `RT_GRAPHICS2D_FILTER_*` value, or NEAREST when
///         @p texture is invalid.
int64_t rt_texture2d_get_filter(void *texture) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    return impl ? impl->filter : RT_GRAPHICS2D_FILTER_NEAREST;
}

/// @brief Set the texture wrap mode (`RT_GRAPHICS2D_WRAP_CLAMP` or `REPEAT`).
/// @details Only the REPEAT constant selects repeating coordinates; every
///          other value normalizes to CLAMP.
/// @param texture Opaque Texture2D or GpuTexture2D handle to update; invalid
///                handles are ignored.
/// @param wrap Requested `RT_GRAPHICS2D_WRAP_*` value.
void rt_texture2d_set_wrap(void *texture, int64_t wrap) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    if (!impl)
        return;
    impl->wrap =
        (wrap == RT_GRAPHICS2D_WRAP_REPEAT) ? RT_GRAPHICS2D_WRAP_REPEAT : RT_GRAPHICS2D_WRAP_CLAMP;
}

/// @brief Get the current wrap mode, defaulting to CLAMP for invalid handles.
/// @param texture Opaque Texture2D or GpuTexture2D handle to query.
/// @return The stored `RT_GRAPHICS2D_WRAP_*` value, or CLAMP when @p texture is
///         invalid.
int64_t rt_texture2d_get_wrap(void *texture) {
    rt_texture2d_impl *impl = texture2d_checked(texture);
    return impl ? impl->wrap : RT_GRAPHICS2D_WRAP_CLAMP;
}

/// @brief Release the source reference in a draw command and zero-wipe the slot.
/// @details Zeroing prevents stale ownership or render state from surviving if
///          the buffer slot is reused by a later command.
/// @param cmd Mutable command slot to release; `NULL` is accepted as a no-op.
static void renderer2d_release_cmd(rt_renderer2d_cmd *cmd) {
    if (!cmd)
        return;
    rt2d_release_ref_slot(&cmd->source);
    memset(cmd, 0, sizeof(*cmd));
}

/// @brief GC finalizer — releases all retained source references and frees the command buffer.
/// @details Clears the implementation magic cookie first, releases each
///          initialized command through `renderer2d_release_cmd`, and then
///          frees the renderer-owned backing allocation.
/// @param obj Finalizing Renderer2D implementation supplied by the runtime
///            object system.
static void renderer2d_finalize(void *obj) {
    rt_renderer2d_impl *renderer = (rt_renderer2d_impl *)obj;
    renderer->magic = 0;
    if (renderer->cmds) {
        for (int64_t i = 0; i < renderer->count; i++)
            renderer2d_release_cmd(&renderer->cmds[i]);
        free(renderer->cmds);
    }
}

//=============================================================================
// Renderer2D
//=============================================================================
// Retained command stream. `Begin` clears + arms the queue; `Draw*` calls append
// commands with the current tint / alpha / blend mode snapshot. `FlushToTarget`
// draws without consuming or deactivating the queue, while `End` draws to an
// optional Canvas, deactivates, and releases every queued source reference.

/// @brief Grow the command buffer to fit at least `needed` entries.
/// @details Doubling-growth with overflow guards at both the element-count level
///          and the byte-count level (so a malicious caller can't wrap the
///          multiplication by `sizeof(rt_renderer2d_cmd)` past `INT64_MAX`).
///          Fresh slots are zero-initialized so new commands never inherit stale
///          pointers or state. A null renderer is treated as already satisfied
///          because append callers reject it before reserving.
/// @param renderer Mutable Renderer2D implementation whose command buffer may
///                 be reallocated.
/// @param needed Minimum number of command slots required.
/// @return Nonzero for a null renderer, sufficient existing capacity, or
///         successful growth; zero on overflow or allocation failure.
static int32_t renderer2d_reserve(rt_renderer2d_impl *renderer, int64_t needed) {
    if (!renderer || needed <= renderer->capacity)
        return 1;
    int64_t cap = renderer->capacity > 0 ? renderer->capacity : RT2D_INITIAL_CAP;
    while (cap < needed) {
        if (cap > INT64_MAX / 2)
            return 0;
        cap *= 2;
    }
    if (cap > INT64_MAX / (int64_t)sizeof(rt_renderer2d_cmd))
        return 0;
    rt_renderer2d_cmd *cmds =
        (rt_renderer2d_cmd *)realloc(renderer->cmds, (size_t)cap * sizeof(rt_renderer2d_cmd));
    if (!cmds)
        return 0;
    memset(cmds + renderer->capacity, 0, (size_t)(cap - renderer->capacity) * sizeof(*cmds));
    renderer->cmds = cmds;
    renderer->capacity = cap;
    return 1;
}

/// @brief Allocate a new Renderer2D with the given initial command-buffer capacity.
/// @details Default render state: no tint (`tint = -1`), full alpha (`alpha = 255`),
///          ALPHA blend mode. The renderer starts inactive; call `Begin` before
///          the first `Draw*`. A nonpositive capacity selects the default of 16,
///          positive requests up to 1,048,576 are preserved, and larger initial
///          requests are capped at that value.
/// @param capacity Requested number of command slots to allocate initially.
/// @return A new inactive Renderer2D handle, or `NULL` if object or command
///         buffer allocation fails.
void *rt_renderer2d_new(int64_t capacity) {
    rt_renderer2d_impl *renderer = (rt_renderer2d_impl *)rt_obj_new_i64(
        RT2D_RENDERER_CLASS_ID, (int64_t)sizeof(rt_renderer2d_impl));
    if (!renderer)
        return NULL;
    renderer->magic = RT2D_RENDERER_MAGIC;
    renderer->capacity = rt2d_initial_capacity(capacity);
    renderer->cmds =
        (rt_renderer2d_cmd *)calloc((size_t)renderer->capacity, sizeof(rt_renderer2d_cmd));
    if (!renderer->cmds) {
        if (rt_obj_release_check0(renderer))
            rt_obj_free(renderer);
        return NULL;
    }
    renderer->tint = -1;
    renderer->alpha = 255;
    renderer->blend_mode = RT_GRAPHICS2D_BLEND_ALPHA;
    rt_obj_set_finalizer(renderer, renderer2d_finalize);
    return renderer;
}

/// @brief Release all queued draw commands and reset the command count to zero.
/// @details Retained sources are released and command slots are wiped, but the
///          allocated buffer and current active/state settings are preserved.
/// @param renderer Opaque Renderer2D handle to clear; invalid handles are
///                 ignored.
void rt_renderer2d_clear(void *renderer) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (!impl)
        return;
    for (int64_t i = 0; i < impl->count; i++)
        renderer2d_release_cmd(&impl->cmds[i]);
    impl->count = 0;
}

/// @brief Clear the command queue and mark the renderer active for new draw commands.
/// @details Any previously retained command sources are released before the
///          active flag is set. Tint, alpha, and blend settings are unchanged.
/// @param renderer Opaque Renderer2D handle to begin; invalid handles are
///                 ignored.
void rt_renderer2d_begin(void *renderer) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (!impl)
        return;
    rt_renderer2d_clear(renderer);
    impl->active = 1;
}

/// @brief Return the number of draw commands currently queued.
/// @param renderer Opaque Renderer2D handle to query.
/// @return The logical command count, or `0` when @p renderer is invalid.
int64_t rt_renderer2d_count(void *renderer) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    return impl ? impl->count : 0;
}

/// @brief Set the multiplicative tint color (`0x00RRGGBB`); negative disables tinting.
/// @details The setting is captured only by commands queued after this call.
/// @param renderer Opaque Renderer2D handle to update; invalid handles are
///                 ignored.
/// @param rgb Tint value; negative disables tinting and nonnegative values are
///            masked to 24 bits.
void rt_renderer2d_set_tint(void *renderer, int64_t rgb) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (impl)
        impl->tint = rgb < 0 ? -1 : (rgb & 0x00FFFFFF);
}

/// @brief Set the alpha scale clamped to `[0, 255]`; 255 = fully opaque.
/// @details The setting is captured only by commands queued after this call.
/// @param renderer Opaque Renderer2D handle to update; invalid handles are
///                 ignored.
/// @param alpha Requested additional alpha multiplier.
void rt_renderer2d_set_alpha(void *renderer, int64_t alpha) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (impl)
        impl->alpha = clamp_u8_i64(alpha);
}

/// @brief Set the blend mode (`RT_GRAPHICS2D_BLEND_*`) clamped to the valid range `[0, 2]`.
/// @details The setting is captured only by commands queued after this call.
/// @param renderer Opaque Renderer2D handle to update; invalid handles are
///                 ignored.
/// @param blend_mode Requested blend-mode value.
void rt_renderer2d_set_blend_mode(void *renderer, int64_t blend_mode) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (impl)
        impl->blend_mode = clamp_i64(blend_mode, 0, 2);
}

/// @brief Append one draw command to the active queue, snapshotting current render
///        state (tint / alpha / blend mode) into the command so later state changes
///        don't retroactively affect already-queued draws.
/// @details Refuses to queue when the renderer isn't active (caller forgot to call
///          `Begin`) or the source is NULL. Capacity-overflow on the command buffer
///          traps rather than silently dropping draws — a silently-dropped draw in
///          a retained command stream is almost always a bug the caller would want
///          to see.
/// @param renderer Mutable validated Renderer2D implementation.
/// @param source_kind Source tag: `0` for Pixels or `1` for Texture2D-compatible
///                    objects.
/// @param source Object retained by the queued command until clear, end, or
///               finalization.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Positive source rectangle width.
/// @param height Positive source rectangle height.
/// @param dst_width Positive destination width used for sampled texture draws.
/// @param dst_height Positive destination height used for sampled texture draws.
/// @param sampler_filter Snapshotted `RT_GRAPHICS2D_FILTER_*` value.
/// @param sampler_wrap Snapshotted `RT_GRAPHICS2D_WRAP_*` value.
static void renderer2d_queue(rt_renderer2d_impl *renderer,
                             int32_t source_kind,
                             void *source,
                             int64_t x,
                             int64_t y,
                             int64_t sx,
                             int64_t sy,
                             int64_t width,
                             int64_t height,
                             int64_t dst_width,
                             int64_t dst_height,
                             int64_t sampler_filter,
                             int64_t sampler_wrap) {
    if (!renderer || renderer->magic != RT2D_RENDERER_MAGIC || !renderer->active || !source)
        return;
    if (width <= 0 || height <= 0 || dst_width <= 0 || dst_height <= 0)
        return;
    if (!renderer2d_reserve(renderer, renderer->count + 1)) {
        rt_trap("Renderer2D: command capacity overflow");
        return;
    }
    rt2d_retain_ref(source);
    rt_renderer2d_cmd *cmd = &renderer->cmds[renderer->count];
    cmd->source = source;
    cmd->source_kind = source_kind;
    cmd->x = x;
    cmd->y = y;
    cmd->sx = sx;
    cmd->sy = sy;
    cmd->width = width;
    cmd->height = height;
    cmd->dst_width = dst_width;
    cmd->dst_height = dst_height;
    cmd->tint = renderer->tint;
    cmd->alpha = renderer->alpha;
    cmd->blend_mode = renderer->blend_mode;
    cmd->sampler_filter = sampler_filter;
    cmd->sampler_wrap = sampler_wrap;
    cmd->angle_deg = 0.0;
    cmd->pivot_x = 0;
    cmd->pivot_y = 0;
    cmd->has_rotation = 0;
    renderer->count++;
}

/// @brief Queue a draw command to blit the full @p pixels buffer at `(x, y)` with current render
/// state.
/// @details Invalid Pixels handles are silently skipped. The queued command
///          retains the source and uses NEAREST/CLAMP sampling metadata. An
///          inactive or invalid renderer also produces no command.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param pixels Valid source Pixels handle to retain.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
void rt_renderer2d_draw_pixels(void *renderer, void *pixels, int64_t x, int64_t y) {
    /* Soft-skip an invalid handle (no trap) to match rt_renderer2d_draw_texture, so
     * a stale handle mid-frame drops the one draw rather than aborting the program. */
    rt_pixels_impl *p = rt_pixels_checked_impl_or_null(pixels);
    if (!p)
        return;
    renderer2d_queue(renderer2d_checked(renderer),
                     0,
                     pixels,
                     x,
                     y,
                     0,
                     0,
                     p->width,
                     p->height,
                     p->width,
                     p->height,
                     RT_GRAPHICS2D_FILTER_NEAREST,
                     RT_GRAPHICS2D_WRAP_CLAMP);
}

/// @brief Queue a draw command to blit the full Texture2D at `(x, y)`, propagating its sampler
/// state.
/// @details Texture2D and GpuTexture2D handles are accepted. The command
///          snapshots the texture dimensions, filter, renderer tint/alpha/blend
///          state, and retains the texture until the queue is cleared.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param texture Valid Texture2D or GpuTexture2D source handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
void rt_renderer2d_draw_texture(void *renderer, void *texture, int64_t x, int64_t y) {
    if (!texture || !texture2d_checked(texture))
        return;
    renderer2d_queue(renderer2d_checked(renderer),
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
                     rt_texture2d_get_filter(texture),
                     rt_texture2d_get_wrap(texture));
}

/// @brief Queue a full-texture draw rotated about the texture center.
/// @details @p x and @p y locate the unrotated texture's top-left corner. The
///          finite angle is reduced modulo 360 degrees, and the integer pivot
///          is `(width / 2, height / 2)`. Invalid inputs or an inactive renderer
///          leave the queue unchanged.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param texture Valid Texture2D or GpuTexture2D source handle.
/// @param x Destination X coordinate before rotation.
/// @param y Destination Y coordinate before rotation.
/// @param angle_deg Signed rotation angle in degrees.
void rt_renderer2d_draw_texture_rotated(
    void *renderer, void *texture, int64_t x, int64_t y, double angle_deg) {
    if (!texture || !texture2d_checked(texture) || !rt2d_normalize_angle_degrees(&angle_deg))
        return;
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (!impl)
        return;
    int64_t width = rt_texture2d_width(texture);
    int64_t height = rt_texture2d_height(texture);
    int64_t before = impl->count;
    renderer2d_queue(impl,
                     1,
                     texture,
                     x,
                     y,
                     0,
                     0,
                     width,
                     height,
                     width,
                     height,
                     rt_texture2d_get_filter(texture),
                     rt_texture2d_get_wrap(texture));
    if (impl->count > before) {
        rt_renderer2d_cmd *cmd = &impl->cmds[impl->count - 1];
        cmd->has_rotation = 1;
        cmd->pivot_x = width / 2;
        cmd->pivot_y = height / 2;
        cmd->angle_deg = angle_deg;
    }
}

/// @brief Queue a full-texture draw rotated about an explicit local pivot.
/// @details @p pivot_x and @p pivot_y are measured from the texture's unrotated
///          top-left corner and are stored without clamping, allowing pivots
///          outside the image. The finite angle is reduced modulo 360 degrees.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param texture Valid Texture2D or GpuTexture2D source handle.
/// @param x Destination X coordinate before rotation.
/// @param y Destination Y coordinate before rotation.
/// @param pivot_x Local X coordinate of the rotation pivot.
/// @param pivot_y Local Y coordinate of the rotation pivot.
/// @param angle_deg Rotation angle in degrees.
void rt_renderer2d_draw_texture_rotated_at(void *renderer,
                                           void *texture,
                                           int64_t x,
                                           int64_t y,
                                           int64_t pivot_x,
                                           int64_t pivot_y,
                                           double angle_deg) {
    if (!texture || !texture2d_checked(texture) || !rt2d_normalize_angle_degrees(&angle_deg))
        return;
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (!impl)
        return;
    int64_t width = rt_texture2d_width(texture);
    int64_t height = rt_texture2d_height(texture);
    int64_t before = impl->count;
    renderer2d_queue(impl,
                     1,
                     texture,
                     x,
                     y,
                     0,
                     0,
                     width,
                     height,
                     width,
                     height,
                     rt_texture2d_get_filter(texture),
                     rt_texture2d_get_wrap(texture));
    if (impl->count > before) {
        rt_renderer2d_cmd *cmd = &impl->cmds[impl->count - 1];
        cmd->has_rotation = 1;
        cmd->pivot_x = pivot_x;
        cmd->pivot_y = pivot_y;
        cmd->angle_deg = angle_deg;
    }
}

/// @brief Queue a scaled full-texture draw, using the texture's filter and wrap state.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param texture Valid Texture2D or GpuTexture2D source handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param width Positive destination width; nonpositive values are ignored.
/// @param height Positive destination height; nonpositive values are ignored.
void rt_renderer2d_draw_texture_scaled(
    void *renderer, void *texture, int64_t x, int64_t y, int64_t width, int64_t height) {
    if (!texture || !texture2d_checked(texture) || width <= 0 || height <= 0)
        return;
    renderer2d_queue(renderer2d_checked(renderer),
                     1,
                     texture,
                     x,
                     y,
                     0,
                     0,
                     rt_texture2d_width(texture),
                     rt_texture2d_height(texture),
                     width,
                     height,
                     rt_texture2d_get_filter(texture),
                     rt_texture2d_get_wrap(texture));
}

/// @brief Queue a texture-region draw. Out-of-bounds source texels obey the texture wrap mode.
/// @details The destination extent equals the requested source extent. Filter,
///          wrap, tint, alpha, and blend state are captured with the command.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param texture Valid Texture2D or GpuTexture2D source handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param sx Source rectangle X coordinate, which may lie outside the image.
/// @param sy Source rectangle Y coordinate, which may lie outside the image.
/// @param width Positive source and destination width.
/// @param height Positive source and destination height.
void rt_renderer2d_draw_texture_region(void *renderer,
                                       void *texture,
                                       int64_t x,
                                       int64_t y,
                                       int64_t sx,
                                       int64_t sy,
                                       int64_t width,
                                       int64_t height) {
    if (!texture || !texture2d_checked(texture) || width <= 0 || height <= 0)
        return;
    renderer2d_queue(renderer2d_checked(renderer),
                     1,
                     texture,
                     x,
                     y,
                     sx,
                     sy,
                     width,
                     height,
                     width,
                     height,
                     rt_texture2d_get_filter(texture),
                     rt_texture2d_get_wrap(texture));
}

/// @brief Queue a draw command to blit the sub-region `(sx, sy, width, height)` of @p pixels at
/// `(x, y)`.
/// @details Unlike `rt_renderer2d_draw_pixels`, invalid Pixels input is checked
///          through the trapping Pixels validator. The raw Pixels path uses
///          NEAREST/CLAMP metadata and clips the source region when executed.
/// @param renderer Opaque Renderer2D handle receiving the command.
/// @param pixels Valid source Pixels handle to retain.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Positive requested source and destination width.
/// @param height Positive requested source and destination height.
void rt_renderer2d_draw_region(void *renderer,
                               void *pixels,
                               int64_t x,
                               int64_t y,
                               int64_t sx,
                               int64_t sy,
                               int64_t width,
                               int64_t height) {
    if (!rt_pixels_checked_impl(pixels, "Renderer2D.DrawRegion: null pixels"))
        return;
    renderer2d_queue(renderer2d_checked(renderer),
                     0,
                     pixels,
                     x,
                     y,
                     sx,
                     sy,
                     width,
                     height,
                     width,
                     height,
                     RT_GRAPHICS2D_FILTER_NEAREST,
                     RT_GRAPHICS2D_WRAP_CLAMP);
}

/// @brief Execute one queued draw command against a Pixels render target.
/// @details Resolves the source (Texture2D → its Pixels, or raw Pixels directly) then calls
///   the rotated blitter, sampled texture blitter, or direct Pixels blitter as
///   required. Rotation and texture commands honor snapshotted filter and wrap
///   state; every path uses the command's tint, alpha, and blend mode.
/// @param cmd Borrowed initialized command to execute.
/// @param target_pixels Borrowed destination Pixels handle modified in place.
static void renderer2d_flush_cmd_to_target(rt_renderer2d_cmd *cmd, void *target_pixels) {
    void *pixels = cmd->source_kind == 1 ? rt_texture2d_get_pixels(cmd->source) : cmd->source;
    if (!pixels)
        return;
    if (cmd->has_rotation) {
        blit_pixels_rotated(target_pixels,
                            cmd->x,
                            cmd->y,
                            pixels,
                            cmd->pivot_x,
                            cmd->pivot_y,
                            cmd->angle_deg,
                            cmd->tint,
                            cmd->alpha,
                            cmd->blend_mode,
                            cmd->sampler_filter,
                            cmd->sampler_wrap);
        return;
    }
    if (cmd->source_kind == 1) {
        blit_pixels_sampled_scaled(target_pixels,
                                   cmd->x,
                                   cmd->y,
                                   pixels,
                                   cmd->sx,
                                   cmd->sy,
                                   cmd->width,
                                   cmd->height,
                                   cmd->dst_width,
                                   cmd->dst_height,
                                   cmd->tint,
                                   cmd->alpha,
                                   cmd->blend_mode,
                                   cmd->sampler_filter,
                                   cmd->sampler_wrap);
        return;
    }
    rt2d_blit_pixels(target_pixels,
                cmd->x,
                cmd->y,
                pixels,
                cmd->sx,
                cmd->sy,
                cmd->width,
                cmd->height,
                cmd->tint,
                cmd->alpha,
                cmd->blend_mode);
}

/// @brief Blit a Pixels buffer to a Canvas surface using the specified blend mode.
/// @details Routes through the appropriate Canvas API function based on blend mode:
///   OPAQUE → `rt_canvas_blit`, ALPHA → `rt_canvas_blit_alpha`, ADD → read-modify-write
///   via `rt_canvas_copy_rect` + `rt2d_blit_pixels(ADD)` + `rt_canvas_blit`. The additive
///   path is the only one that needs a round-trip through a temporary Pixels
///   buffer. The additive path clips negative and oversized destinations before
///   copying the Canvas region. Unsupported blend values follow the alpha path.
/// @param canvas Borrowed destination Canvas handle modified in place.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param pixels Borrowed source Pixels handle.
/// @param blend_mode Snapshotted `RT_GRAPHICS2D_BLEND_*` value.
static void renderer2d_blit_pixels_to_canvas(
    void *canvas, int64_t x, int64_t y, void *pixels, int64_t blend_mode) {
    if (!canvas || !pixels)
        return;
    if (blend_mode == RT_GRAPHICS2D_BLEND_OPAQUE) {
        rt_canvas_blit(canvas, x, y, pixels);
        return;
    }
    if (blend_mode != RT_GRAPHICS2D_BLEND_ADD) {
        rt_canvas_blit_alpha(canvas, x, y, pixels);
        return;
    }

    int64_t dx = x;
    int64_t dy = y;
    int64_t sx = 0;
    int64_t sy = 0;
    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    if (width <= 0 || height <= 0)
        return;
    int64_t canvas_width = rt_canvas_width(canvas);
    int64_t canvas_height = rt_canvas_height(canvas);
    if (canvas_width <= 0 || canvas_height <= 0)
        return;
    if (dx < 0) {
        int64_t shift = dx == INT64_MIN ? INT64_MAX : -dx;
        if (shift >= width)
            return;
        sx = shift;
        width -= shift;
        dx = 0;
    }
    if (dy < 0) {
        int64_t shift = dy == INT64_MIN ? INT64_MAX : -dy;
        if (shift >= height)
            return;
        sy = shift;
        height -= shift;
        dy = 0;
    }
    if (dx >= canvas_width || dy >= canvas_height)
        return;
    if (width > canvas_width - dx)
        width = canvas_width - dx;
    if (height > canvas_height - dy)
        height = canvas_height - dy;
    if (width <= 0 || height <= 0)
        return;

    void *dest = rt_canvas_copy_rect(canvas, dx, dy, width, height);
    if (!dest)
        return;
    rt2d_blit_pixels(dest, 0, 0, pixels, sx, sy, width, height, -1, 255, RT_GRAPHICS2D_BLEND_ADD);
    rt_canvas_blit(canvas, dx, dy, dest);
    rt2d_release_ref_slot(&dest);
}

/// @brief Execute all queued draw commands against an offscreen pixel target.
/// @details Does NOT deactivate the renderer — use `rt_renderer2d_end` for the
///   Canvas path or `rt_renderer2d_clear` to reset without flushing. It also
///   does not clear the queue, so repeated calls redraw the same commands.
/// @param renderer Opaque Renderer2D handle whose queued commands are executed.
/// @param target Opaque RenderTarget2D or Surface2D destination handle.
void rt_renderer2d_flush_to_target(void *renderer, void *target) {
    if (!renderer || !target)
        return;
    void *target_pixels = rt_rendertarget2d_get_pixels(target);
    if (!target_pixels)
        return;
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (!impl)
        return;
    for (int64_t i = 0; i < impl->count; i++)
        renderer2d_flush_cmd_to_target(&impl->cmds[i], target_pixels);
}

/// @brief Flush the queued command stream to a Canvas, then deactivate the queue.
/// @details For every command:
///          1. Resolve the source Pixels (Texture2D → `.pixels`, Pixels → self).
///          2. Rasterize rotations into a tightly bounded temporary image, or
///             materialize texture sampling/scaling/wrapping when the direct
///             Canvas path cannot represent those settings.
///          3. Clip and copy raw-Pixels subregions when necessary.
///          4. Apply snapshotted tint and alpha to non-rotated results.
///          5. Composite with the snapshotted opaque, alpha, or additive blend
///             mode and release all temporary images.
///
///          Whether or not @p canvas is non-null, a successfully validated
///          active renderer is then deactivated and cleared, releasing every
///          retained source. A subsequent `Begin` is required before queueing
///          more draws. Individual commands whose temporary allocation or
///          geometry validation fails are skipped. If only the optional
///          tint/alpha clone allocation fails, the current implementation
///          falls back to the unprocessed image for that command.
/// @param renderer Opaque active Renderer2D handle to finish.
/// @param canvas Borrowed destination Canvas handle, or `NULL` to discard the
///               queue while still ending the active recording pass.
void rt_renderer2d_end(void *renderer, void *canvas) {
    rt_renderer2d_impl *impl = renderer2d_checked(renderer);
    if (!impl)
        return;
    if (!impl->active)
        return;
    if (canvas) {
        for (int64_t i = 0; i < impl->count; i++) {
            rt_renderer2d_cmd *cmd = &impl->cmds[i];
            void *pixels =
                cmd->source_kind == 1 ? rt_texture2d_get_pixels(cmd->source) : cmd->source;
            if (!pixels)
                continue;
            void *region = NULL;
            void *draw_pixels = pixels;
            int64_t blit_x = cmd->x;
            int64_t blit_y = cmd->y;
            if (cmd->has_rotation) {
                double angle_deg = cmd->angle_deg;
                if (!rt2d_normalize_angle_degrees(&angle_deg))
                    continue;
                int64_t sw = rt_pixels_width(pixels);
                int64_t sh = rt_pixels_height(pixels);
                double radians = angle_deg * (double)RT2D_PI / 180.0;
                double cos_a = cos(radians);
                double sin_a = sin(radians);
                double corners[4][2] = {
                    {-(double)cmd->pivot_x, -(double)cmd->pivot_y},
                    {(double)sw - (double)cmd->pivot_x, -(double)cmd->pivot_y},
                    {(double)sw - (double)cmd->pivot_x, (double)sh - (double)cmd->pivot_y},
                    {-(double)cmd->pivot_x, (double)sh - (double)cmd->pivot_y}};
                double min_x = 1.0e30;
                double min_y = 1.0e30;
                double max_x = -1.0e30;
                double max_y = -1.0e30;
                for (int j = 0; j < 4; j++) {
                    double rx = corners[j][0] * cos_a - corners[j][1] * sin_a;
                    double ry = corners[j][0] * sin_a + corners[j][1] * cos_a;
                    if (rx < min_x)
                        min_x = rx;
                    if (rx > max_x)
                        max_x = rx;
                    if (ry < min_y)
                        min_y = ry;
                    if (ry > max_y)
                        max_y = ry;
                }
                int64_t x0;
                int64_t x1;
                int64_t y0;
                int64_t y1;
                if (!rt2d_floor_i64_checked(min_x, &x0) || !rt2d_ceil_i64_checked(max_x, &x1) ||
                    !rt2d_floor_i64_checked(min_y, &y0) || !rt2d_ceil_i64_checked(max_y, &y1))
                    continue;
                int64_t rw = x1 - x0;
                int64_t rh = y1 - y0;
                if (rw <= 0 || rh <= 0 || !rt2d_checked_count(rw, rh, 4, NULL))
                    continue;
                region = rt_pixels_new(rw, rh);
                if (!region)
                    continue;
                blit_pixels_rotated(region,
                                    -x0 - cmd->pivot_x,
                                    -y0 - cmd->pivot_y,
                                    pixels,
                                    cmd->pivot_x,
                                    cmd->pivot_y,
                                    angle_deg,
                                    cmd->tint,
                                    cmd->alpha,
                                    RT_GRAPHICS2D_BLEND_OPAQUE,
                                    cmd->sampler_filter,
                                    cmd->sampler_wrap);
                draw_pixels = region;
                blit_x = rt2d_saturating_add_i64(rt2d_saturating_add_i64(cmd->x, cmd->pivot_x), x0);
                blit_y = rt2d_saturating_add_i64(rt2d_saturating_add_i64(cmd->y, cmd->pivot_y), y0);
            } else if (cmd->source_kind == 1 &&
                       (cmd->sx != 0 || cmd->sy != 0 || cmd->width != rt_pixels_width(pixels) ||
                        cmd->height != rt_pixels_height(pixels) || cmd->dst_width != cmd->width ||
                        cmd->dst_height != cmd->height ||
                        cmd->sampler_wrap == RT_GRAPHICS2D_WRAP_REPEAT ||
                        cmd->sampler_filter == RT_GRAPHICS2D_FILTER_LINEAR)) {
                if (!rt2d_checked_count(cmd->dst_width, cmd->dst_height, 4, NULL))
                    continue;
                region = rt_pixels_new(cmd->dst_width, cmd->dst_height);
                if (!region)
                    continue;
                blit_pixels_sampled_scaled(region,
                                           0,
                                           0,
                                           pixels,
                                           cmd->sx,
                                           cmd->sy,
                                           cmd->width,
                                           cmd->height,
                                           cmd->dst_width,
                                           cmd->dst_height,
                                           -1,
                                           255,
                                           RT_GRAPHICS2D_BLEND_OPAQUE,
                                           cmd->sampler_filter,
                                           cmd->sampler_wrap);
                draw_pixels = region;
            } else if (cmd->sx != 0 || cmd->sy != 0 || cmd->width != rt_pixels_width(pixels) ||
                       cmd->height != rt_pixels_height(pixels)) {
                int64_t sx = cmd->sx;
                int64_t sy = cmd->sy;
                int64_t width = cmd->width;
                int64_t height = cmd->height;
                if (!clip_region_to_source(pixels, &blit_x, &blit_y, &sx, &sy, &width, &height))
                    continue;
                region = rt2d_copy_region_pixels(pixels, sx, sy, width, height);
                draw_pixels = region;
            }
            void *processed = processed_pixels_or_null(draw_pixels,
                                                       cmd->has_rotation ? -1 : cmd->tint,
                                                       cmd->has_rotation ? 255 : cmd->alpha);
            if (processed)
                draw_pixels = processed;
            if (draw_pixels)
                renderer2d_blit_pixels_to_canvas(
                    canvas, blit_x, blit_y, draw_pixels, cmd->blend_mode);
            rt2d_release_ref_slot(&processed);
            rt2d_release_ref_slot(&region);
        }
    }
    impl->active = 0;
    rt_renderer2d_clear(renderer);
}
