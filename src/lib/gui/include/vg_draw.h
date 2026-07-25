//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_draw.h
// Purpose: Shared anti-aliased 2D drawing core for the GUI toolkit. Provides
//          coverage-based rounded rectangles, discs, rings, and lines that all
//          widget paint code routes through, so every widget gets the same
//          crisp, modern rendering instead of hand-rolled hard-edged shapes.
// Key invariants:
//   - Per-pixel coverage math is integer/fixed-point only (16.16 / Q8). The
//     only floating point is a handful of setup-time roundings, so output is
//     bit-identical across macOS/Windows/Linux (determinism contract).
//   - Colours are 24-bit RGB (0x00RRGGBB); the input alpha channel is ignored.
//     Computed edge coverage is the blend alpha. Opaque interiors are written
//     via vgfx_fill_rect; anti-aliased rims via vgfx_pset_alpha.
//   - Operates in framebuffer pixels. The GUI layer keeps the vgfx coord scale
//     at 1.0, so there is no HiDPI block-expansion to fight; callers pass
//     geometry already scaled for the target (radius etc. in physical px).
// Ownership/Lifetime:
//   - Stateless; draws directly into the supplied window's framebuffer. No
//     allocation, no global state (the P2 shadow cache is the sole exception).
// Links: lib/gui/src/core/vg_draw.c,
//        lib/graphics/include/vgfx.h (vgfx_pset_alpha, vgfx_fill_rect),
//        lib/gui/src/font/vg_raster.c (coverage technique, float variant)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vgfx.h"
#include <stdint.h>

/// @file
/// @brief Declares the GUI toolkit's deterministic anti-aliased 2D primitives.
/// @details All geometry is expressed in framebuffer pixels and all colors are 24-bit RGB.
/// Implementations use integer/fixed-point coverage calculations so identical inputs produce
/// bit-identical rounded rectangles, circles, lines, shadows, gradients, and highlights across
/// supported platforms.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Anti-aliased primitives
//
// All coordinates are in framebuffer pixels and may be fractional. All colours
// are 0x00RRGGBB (the alpha byte is ignored; edge coverage supplies the alpha).
//=============================================================================

/// @brief Fill an anti-aliased rounded rectangle.
/// @details The interior is filled opaque via scanline rectangles; only the
///          four corner arcs are anti-aliased ("AA the rim only"), so cost is
///          independent of the rectangle's area and interior pixels are written
///          exactly once (no double-blend seams). A radius <= 0 degenerates to
///          a plain filled rectangle. The radius is clamped to min(w,h)/2.
/// @param win    Target window.
/// @param x,y    Top-left corner.
/// @param w,h    Size in pixels.
/// @param radius Corner radius in pixels.
/// @param rgb    Fill colour (0x00RRGGBB).
void vg_draw_round_rect_fill(vgfx_window_t win, float x, float y, float w, float h, float radius,
                             uint32_t rgb);

/// @brief Stroke an anti-aliased rounded-rectangle outline.
/// @details The stroke sits inside the geometric edge so the outer radius
///          matches vg_draw_round_rect_fill. Straight edges are crisp; corners
///          are anti-aliased rings. A radius <= 0 strokes a plain rectangle.
/// @param win      Target window.
/// @param x,y,w,h  Rectangle bounds.
/// @param radius   Corner radius in pixels.
/// @param stroke_w Stroke width in pixels (clamped to >= 1).
/// @param rgb      Stroke colour (0x00RRGGBB).
void vg_draw_round_rect_stroke(vgfx_window_t win, float x, float y, float w, float h, float radius,
                               float stroke_w, uint32_t rgb);

/// @brief Fill an anti-aliased disc (solid circle).
/// @param win   Target window.
/// @param cx,cy Centre (may be fractional for smooth positioning).
/// @param r     Radius in pixels.
/// @param rgb   Fill colour (0x00RRGGBB).
void vg_draw_disc_fill(vgfx_window_t win, float cx, float cy, float r, uint32_t rgb);

/// @brief Stroke an anti-aliased circle outline.
/// @param win      Target window.
/// @param cx,cy    Centre.
/// @param r        Radius in pixels (outer edge of the stroke).
/// @param stroke_w Stroke width in pixels (clamped to >= 1).
/// @param rgb      Stroke colour (0x00RRGGBB).
void vg_draw_circle_stroke(vgfx_window_t win, float cx, float cy, float r, float stroke_w,
                           uint32_t rgb);

/// @brief Draw an anti-aliased line segment with round caps.
/// @param win      Target window.
/// @param x0,y0    Start point.
/// @param x1,y1    End point.
/// @param stroke_w Line thickness in pixels (clamped to >= 1).
/// @param rgb      Line colour (0x00RRGGBB).
void vg_draw_line_aa(vgfx_window_t win, float x0, float y0, float x1, float y1, float stroke_w,
                     uint32_t rgb);

//=============================================================================
// Depth & polish (elevation, gradient, highlight)
//=============================================================================

/// @brief Draw a soft (blurred) drop shadow behind a rounded-rect silhouette.
/// @details The silhouette is rasterised and blurred with a separable box blur
///          run three times (a near-Gaussian, integer-only filter), then
///          composited at the given offset and opacity. Results are cached by
///          (w, h, radius, blur) so repeated frames and position-only moves are
///          cheap. Call this BEFORE painting the surface it sits under.
/// @param win        Target window.
/// @param x,y,w,h    Bounds of the surface casting the shadow.
/// @param radius     Corner radius of the silhouette.
/// @param blur       Blur radius in pixels (<= 0 draws nothing).
/// @param dx,dy      Shadow offset in pixels.
/// @param alpha      Peak shadow opacity (0-255).
/// @param shadow_rgb Shadow colour (0x00RRGGBB), usually near-black.
void vg_draw_round_rect_shadow(vgfx_window_t win, float x, float y, float w, float h, float radius,
                               float blur, int dx, int dy, uint8_t alpha, uint32_t shadow_rgb);

/// @brief Fill an anti-aliased rounded rectangle with a vertical gradient.
/// @details Top colour at the top edge, bottom colour at the bottom edge. Used
///          for the subtle "raised" sheen on buttons and cards; pass two close
///          colours for a refined effect (see vg_gradient_theme strength).
/// @param win        Target window.
/// @param x,y,w,h    Rectangle bounds.
/// @param radius     Corner radius in pixels.
/// @param top_rgb    Colour at the top edge (0x00RRGGBB).
/// @param bottom_rgb Colour at the bottom edge (0x00RRGGBB).
void vg_draw_round_rect_gradient_v(vgfx_window_t win, float x, float y, float w, float h,
                                   float radius, uint32_t top_rgb, uint32_t bottom_rgb);

/// @brief Draw a 1px inner highlight along the top edge between the corners.
/// @details The classic "light from above" cue. Pass a lightened surface colour.
/// @param win    Target window.
/// @param x,y    Top-left of the surface.
/// @param w      Surface width.
/// @param radius Corner radius (the highlight spans between the corner arcs).
/// @param rgb    Highlight colour (0x00RRGGBB).
void vg_draw_inner_highlight_top(vgfx_window_t win, float x, float y, float w, float radius,
                                 uint32_t rgb);

#ifdef __cplusplus
}
#endif
