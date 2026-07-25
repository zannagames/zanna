//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics2d_surface.inc
/// @brief RenderTarget2D and Surface2D offscreen pixel-surface entry points.
///
/// @details This include fragment is consumed only by rt_graphics2d.c and
/// keeps surface allocation, finalization, draw, and sample behavior grouped
/// apart from renderer, material, path, and extended utility classes.
/// RenderTarget2D and Surface2D have distinct runtime identities but share the
/// same validated implementation layout and public operations.
///
// File: src/runtime/graphics/2d/rt_graphics2d_surface.inc
// Purpose: RenderTarget2D and Surface2D offscreen pixel-surface entry points.
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
// RenderTarget2D / Surface2D
//=============================================================================
// Offscreen RGBA surface. Wraps a `Pixels` buffer and exposes alpha-aware
// DrawPixels / DrawRegion that route through the shared `rt2d_blit_pixels` blitter.

/// @brief GC finalizer — releases the owned Pixels buffer.
/// @details Clears the implementation magic cookie before releasing the
///          separately allocated backing image. Runtime object storage itself
///          is reclaimed by the object system after the callback.
/// @param obj Finalizing RenderTarget2D or Surface2D implementation supplied
///            by the runtime object system.
static void rendertarget2d_finalize(void *obj) {
    rt_rendertarget2d_impl *target = (rt_rendertarget2d_impl *)obj;
    target->magic = 0;
    rt2d_release_ref_slot(&target->pixels);
}

/// @brief Allocate a pixel-backed surface with a selected runtime class.
/// @details Dimensions must be positive, no greater than `RT2D_MAX_DIM`, and
///          safe for a four-byte-per-pixel allocation. Invalid dimensions
///          raise a runtime trap using @p api. The implementation owns the
///          Pixels reference created on success.
/// @param width Requested backing-image width in pixels.
/// @param height Requested backing-image height in pixels.
/// @param class_id Runtime class identifier for RenderTarget2D or Surface2D.
/// @param api Trap message used for invalid dimensions; `NULL` selects the
///            RenderTarget2D constructor message.
/// @return A new initialized surface handle, or `NULL` after invalid input or
///         allocation failure.
static void *rendertarget2d_new_with_class(int64_t width,
                                           int64_t height,
                                           int64_t class_id,
                                           const char *api) {
    if (!rt2d_checked_count(width, height, 4, NULL)) {
        rt_trap(api ? api : "RenderTarget2D.New: invalid dimensions");
        return NULL;
    }
    rt_rendertarget2d_impl *target =
        (rt_rendertarget2d_impl *)rt_obj_new_i64(class_id, (int64_t)sizeof(rt_rendertarget2d_impl));
    if (!target)
        return NULL;
    target->magic = RT2D_RENDERTARGET_MAGIC;
    target->pixels = rt_pixels_new(width, height);
    if (!target->pixels) {
        if (rt_obj_release_check0(target))
            rt_obj_free(target);
        return NULL;
    }
    rt_obj_set_finalizer(target, rendertarget2d_finalize);
    return target;
}

/// @brief Allocate a new offscreen render target sized `width × height` (RGBA).
/// @details Validates the dimensions through `rt2d_checked_count` (caps at RT2D_MAX_DIM
///          and rejects overflowing byte counts) and traps on invalid inputs — the
///          caller gets a clear error instead of a silent NULL return for a bad
///          size. The owned `Pixels` buffer is allocated separately; if that
///          allocation fails, the partially-constructed impl is released cleanly.
/// @param width Requested positive backing-image width.
/// @param height Requested positive backing-image height.
/// @return RenderTarget2D handle, or NULL on allocation failure (trap already
///         fired for invalid dimensions).
void *rt_rendertarget2d_new(int64_t width, int64_t height) {
    return rendertarget2d_new_with_class(
        width, height, RT2D_RENDERTARGET_CLASS_ID, "RenderTarget2D.New: invalid dimensions");
}

/// @brief Allocate a Surface2D with distinct runtime type identity and RenderTarget2D behavior.
/// @details Uses the same dimension and allocation validation as
///          `rt_rendertarget2d_new`, but reports Surface2D in its invalid-size
///          trap.
/// @param width Requested positive backing-image width.
/// @param height Requested positive backing-image height.
/// @return A new Surface2D handle, or `NULL` after invalid input or allocation
///         failure.
void *rt_surface2d_new(int64_t width, int64_t height) {
    return rendertarget2d_new_with_class(
        width, height, RT2D_SURFACE_CLASS_ID, "Surface2D.New: invalid dimensions");
}

/// @brief Return the width of the render target's backing Pixels buffer in pixels, or 0 if invalid.
/// @param target Opaque RenderTarget2D or Surface2D handle to query.
/// @return The positive backing-image width, or `0` when @p target is invalid.
int64_t rt_rendertarget2d_width(void *target) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    return impl ? rt_pixels_width(impl->pixels) : 0;
}

/// @brief Return the height of the render target's backing Pixels buffer in pixels, or 0 if
/// invalid.
/// @param target Opaque RenderTarget2D or Surface2D handle to query.
/// @return The positive backing-image height, or `0` when @p target is invalid.
int64_t rt_rendertarget2d_height(void *target) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    return impl ? rt_pixels_height(impl->pixels) : 0;
}

/// @brief Return the backing Pixels buffer (borrowed reference — do not release).
/// @param target Opaque RenderTarget2D or Surface2D handle to query.
/// @return The borrowed backing Pixels handle, or `NULL` when @p target is
///         invalid. The reference remains owned by the surface and is replaced
///         by resize or released at finalization.
void *rt_rendertarget2d_get_pixels(void *target) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    return impl ? impl->pixels : NULL;
}

/// @brief Fill the entire render target with one RGBA value.
/// @details Accepts raw `0xRRGGBBAA` or an explicitly tagged Color value using
///          the same conversion as `rt_pixels_fill`. Invalid surface handles
///          are ignored.
/// @param target Opaque RenderTarget2D or Surface2D handle to clear.
/// @param rgba Raw or tagged RGBA fill value.
void rt_rendertarget2d_clear(void *target, int64_t rgba) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    if (!impl)
        return;
    rt_pixels_fill(impl->pixels, rgba);
}

/// @brief Replace the owned Pixels buffer with a fresh `width × height` allocation.
/// @details Traps on invalid dimensions, no-ops on allocation failure (leaves the
///          existing buffer intact). The replacement is allocated before the
///          old owned reference is released, then its newly created reference
///          is transferred into the surface slot. Existing contents are not
///          copied.
/// @param target Opaque RenderTarget2D or Surface2D handle to resize; invalid
///               handles are ignored.
/// @param width Requested positive replacement width.
/// @param height Requested positive replacement height.
void rt_rendertarget2d_resize(void *target, int64_t width, int64_t height) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    if (!impl)
        return;
    if (!rt2d_checked_count(width, height, 4, NULL)) {
        rt_trap("RenderTarget2D.Resize: invalid dimensions");
        return;
    }
    void *pixels = rt_pixels_new(width, height);
    if (!pixels)
        return;
    rt2d_release_ref_slot(&impl->pixels);
    impl->pixels = pixels;
}

/// @brief Blit @p pixels onto the render target at `(x, y)` using straight-alpha compositing.
/// @details Covers the full source Pixels extent (no region selection). Clips to the target
///   bounds automatically. Uses ALPHA blend mode with no tint and full opacity.
/// @param target Opaque RenderTarget2D or Surface2D destination handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param pixels Borrowed source Pixels handle; `NULL` is ignored.
void rt_rendertarget2d_draw_pixels(void *target, int64_t x, int64_t y, void *pixels) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    if (!impl || !pixels)
        return;
    rt2d_blit_pixels(impl->pixels,
                x,
                y,
                pixels,
                0,
                0,
                rt_pixels_width(pixels),
                rt_pixels_height(pixels),
                -1,
                255,
                RT_GRAPHICS2D_BLEND_ALPHA);
}

/// @brief Blit a sub-rectangle `(sx, sy, width, height)` of @p pixels onto the target at `(x, y)`.
/// @details Selects a region of the source Pixels and blits it with straight-alpha compositing,
///   no tint, and full opacity. Both source and destination coordinates are clipped to their
///   respective buffer bounds by `rt2d_blit_pixels`.
/// @param target Opaque RenderTarget2D or Surface2D destination handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param pixels Borrowed source Pixels handle; `NULL` is ignored.
/// @param sx Source rectangle X coordinate.
/// @param sy Source rectangle Y coordinate.
/// @param width Requested source and destination width before clipping.
/// @param height Requested source and destination height before clipping.
void rt_rendertarget2d_draw_region(void *target,
                                   int64_t x,
                                   int64_t y,
                                   void *pixels,
                                   int64_t sx,
                                   int64_t sy,
                                   int64_t width,
                                   int64_t height) {
    rt_rendertarget2d_impl *impl = rendertarget2d_checked(target);
    if (!impl || !pixels)
        return;
    rt2d_blit_pixels(
        impl->pixels, x, y, pixels, sx, sy, width, height, -1, 255, RT_GRAPHICS2D_BLEND_ALPHA);
}
