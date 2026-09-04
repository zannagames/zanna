//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_canvas3d_coords.inc
// Purpose: Pure physical<->public coordinate conversions for a Canvas3D
//          window. Every Canvas3D space (Width/Height, Mouse.X/Y, the 2D
//          overlay projection) is derived from ONE scale: the window's live
//          physical extent divided by its public extent, which is exactly the
//          scale vgfx_get_size() and vgfx_mouse_pos() apply (ADR 0242,
//          fullscreen invariant). Kept dependency-free so the contract can be
//          unit-tested without a display or a platform backend.
// Key invariants:
//   - The public extent always carries the framebuffer's aspect ratio; the
//     lender Canvas's designed extent is never substituted for it.
//   - physical_to_public(physical_extent - 1) == public_extent - 1 on both
//     axes (mouse space == Width/Height space).
//   - Scale is uniform and clamped to [1, 16]; degenerate windows use 1.
// Ownership/Lifetime:
//   - Stateless; borrows the vgfx window for the duration of each call.
// Links: rt_canvas3d.c (includes this file), src/tests/runtime/
//        RTCanvas3DCoordsContractTests.cpp, docs/adr/0242.
//
//===----------------------------------------------------------------------===//

/// @brief Return the active physical-to-public coordinate scale for a window.
/// @details Derived from the live physical and public extents, so it is by construction the
///          same scale `vgfx_get_size()` and `vgfx_mouse_pos()` apply. Canvas3D pins the window
///          coordinate scale to the backing scale at creation and adoption, so this normally
///          equals `vgfx_window_get_scale()`; deriving it keeps the event path and the sampled
///          `vgfx_mouse_pos()` path in one space even if something else adjusts the scale.
/// @param window Window whose current public extents are inspected; NULL implies scale 1.
/// @return Uniform public coordinate scale, clamped to the supported `1..16` range.
static float canvas3d_coords_public_scale(vgfx_window_t window) {
    int32_t logical_w = 0;
    int32_t logical_h = 0;
    int32_t physical_w;
    int32_t physical_h;
    double sx = 0.0;
    double sy = 0.0;
    double scale;

    if (!window || !vgfx_get_size(window, &logical_w, &logical_h))
        return 1.0f;
    physical_w = vgfx_window_get_width(window);
    physical_h = vgfx_window_get_height(window);
    if (logical_w > 0 && physical_w > 0)
        sx = (double)physical_w / (double)logical_w;
    if (logical_h > 0 && physical_h > 0)
        sy = (double)physical_h / (double)logical_h;
    scale = sx > 0.0 ? sx : sy;
    if (sy > 0.0 && (scale <= 0.0 || sy < scale))
        scale = sy;
    if (!isfinite(scale) || scale < 1.0)
        scale = 1.0;
    if (scale > 16.0)
        scale = 16.0;
    return (float)scale;
}

/// @brief Estimate a physical backing size for a requested public size.
/// @param window Window whose scale is consulted; NULL implies scale 1.
/// @param logical Public dimension in canvas units.
/// @return Rounded physical dimension. Non-positive or overflow-prone inputs are returned
/// unchanged.
static int32_t canvas3d_coords_scale_logical_size(vgfx_window_t window, int32_t logical) {
    float scale;

    if (logical <= 0)
        return logical;
    scale = window ? canvas3d_coords_public_scale(window) : 1.0f;
    if (!isfinite(scale) || scale < 1.0f)
        scale = 1.0f;
    if ((double)logical > (double)INT32_MAX / (double)scale)
        return logical;
    return (int32_t)((double)logical * (double)scale + 0.5);
}

/// @brief Convert a physical framebuffer dimension back to the public space.
/// @param window Window whose scale is consulted; NULL implies scale 1.
/// @param physical Physical framebuffer dimension in backing pixels.
/// @return Rounded public dimension. Non-positive inputs are returned unchanged.
static int32_t canvas3d_coords_unscale_physical_size(vgfx_window_t window, int32_t physical) {
    float scale;

    if (physical <= 0)
        return physical;
    scale = window ? canvas3d_coords_public_scale(window) : 1.0f;
    if (!isfinite(scale) || scale < 1.0f)
        scale = 1.0f;
    return (int32_t)((double)physical / (double)scale + 0.5);
}

/// @brief Translate a physical (backing-pixel) event point into the public coordinate space.
/// @details Platform mouse events carry physical backing-pixel coordinates. Dividing by the
///          derived public scale lands them in the same space as `Canvas3D.Width/Height` and
///          the overlay projection. Truncation (not rounding) matches `vgfx_mouse_pos()` bounds
///          semantics: the last physical column maps to `public_width - 1`.
/// @param window Window whose scale converts backing pixels to public units; NULL implies 1.
/// @param x Physical horizontal event coordinate.
/// @param y Physical vertical event coordinate.
/// @param out_x Receives the public horizontal coordinate (required).
/// @param out_y Receives the public vertical coordinate (required).
static void canvas3d_coords_physical_to_public(
    vgfx_window_t window, int32_t x, int32_t y, int32_t *out_x, int32_t *out_y) {
    float scale = window ? canvas3d_coords_public_scale(window) : 1.0f;
    if (!isfinite(scale) || scale < 0.001f)
        scale = 1.0f;
    if (out_x)
        *out_x = (int32_t)((double)x / (double)scale);
    if (out_y)
        *out_y = (int32_t)((double)y / (double)scale);
}

/// @brief Report whether the window's live extent differs from a canvas's cached extent.
/// @details The public extent can change WITHOUT a RESIZE event: `vgfx_set_coord_scale` rewrites
///          the scale `vgfx_get_size()`/`vgfx_mouse_pos()` apply, and the only event the platform
///          emits is for a physical size change. A Canvas3D that pumps the window therefore
///          re-derives its extent every poll (ADR 0242, borrower self-heal) so `Width/Height`, the
///          overlay projection, and the mouse can never drift apart, whoever wrote the window.
/// @param window Window whose live public and physical extents are read; NULL reports no drift.
/// @param cached_w Cached public width.
/// @param cached_h Cached public height.
/// @param cached_pw Cached physical (framebuffer) width.
/// @param cached_ph Cached physical (framebuffer) height.
/// @param out_w Receives the live public width (required).
/// @param out_h Receives the live public height (required).
/// @param out_pw Receives the live physical width (required).
/// @param out_ph Receives the live physical height (required).
/// @return 1 when the live extents are positive and any of the four differ from the cache.
static int canvas3d_coords_extent_drifted(vgfx_window_t window,
                                          int32_t cached_w,
                                          int32_t cached_h,
                                          int32_t cached_pw,
                                          int32_t cached_ph,
                                          int32_t *out_w,
                                          int32_t *out_h,
                                          int32_t *out_pw,
                                          int32_t *out_ph) {
    int32_t live_w = 0;
    int32_t live_h = 0;
    int32_t live_pw;
    int32_t live_ph;

    if (!window || !out_w || !out_h || !out_pw || !out_ph)
        return 0;
    if (!vgfx_get_size(window, &live_w, &live_h) || live_w <= 0 || live_h <= 0)
        return 0;
    live_pw = vgfx_window_get_width(window);
    live_ph = vgfx_window_get_height(window);
    if (live_pw <= 0 || live_ph <= 0)
        return 0;
    *out_w = live_w;
    *out_h = live_h;
    *out_pw = live_pw;
    *out_ph = live_ph;
    return live_w != cached_w || live_h != cached_h || live_pw != cached_pw || live_ph != cached_ph;
}
