//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_canvas_integration.c
// Purpose: Font-to-canvas integration — renders rasterised glyph alpha bitmaps
//          onto ZannaGFX canvas surfaces using per-pixel alpha blending.
// Key invariants:
//   - vg_canvas_draw_glyph uses direct framebuffer access; it respects the
//     window's active clip rectangle when vgfx_get_clip reports one.
//   - vg_canvas_draw_glyph_pset is a slower fallback for contexts where
//     framebuffer access is unavailable; it uses simple alpha threshold blending.
//   - Both functions treat the canvas parameter as a vgfx_window_t handle.
// Ownership/Lifetime:
//   - Neither function takes ownership of any parameter.
// Links: lib/gui/include/vg_font.h,
//        lib/gui/src/font/vg_raster.c
//
//===----------------------------------------------------------------------===//
#include "../../include/vg_font.h"
#include "vgfx.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "vg_gamma_tables.inc"

/// @file
/// @brief Bridges rasterized font coverage maps to ZannaGFX canvas drawing.
/// @details The primary path clips and blends directly into an RGBA framebuffer, optionally in
/// linear light. A thresholded `vgfx_pset` implementation remains available for canvas backends
/// that do not expose framebuffer memory.

/// @brief Return whether gamma-correct linear-light glyph blending is active.
/// @details Default on; the ZANNA_GUI_TEXT_GAMMA=off environment escape hatch
///          restores the legacy sRGB-space blend (checked once per process).
/// @return Nonzero for gamma-correct blending, or zero for legacy sRGB-space blending.
static int vg_text_gamma_blend_enabled(void) {
    static int s_state = -1;
    if (s_state < 0) {
        const char *setting = getenv("ZANNA_GUI_TEXT_GAMMA");
        int off = setting && (setting[0] == '0' ||
                              ((setting[0] == 'o' || setting[0] == 'O') &&
                               (setting[1] == 'f' || setting[1] == 'F')));
        s_state = off ? 0 : 1;
    }
    return s_state;
}

//=============================================================================
// Canvas Glyph Drawing
//=============================================================================

/// @brief Draw a glyph alpha bitmap onto a vgfx canvas using per-pixel alpha
///        blending with direct framebuffer access.
///
/// @details Respects the window's active clip rectangle. Pixels with
///          alpha == 255 are written without blending. Colors are in 0xRRGGBB
///          format; alpha is sourced from the bitmap.
///
/// @param canvas  vgfx_window_t handle to draw into.
/// @param x       Left edge of the glyph in canvas coordinates.
/// @param y       Top edge of the glyph in canvas coordinates.
/// @param bitmap  8-bit alpha coverage bitmap (width × height bytes).
/// @param width   Width of the bitmap in pixels.
/// @param height  Height of the bitmap in pixels.
/// @param color   Glyph foreground colour in 0xRRGGBB format.
void vg_canvas_draw_glyph(
    void *canvas, int x, int y, const uint8_t *bitmap, int width, int height, uint32_t color) {
    if (!canvas || !bitmap || width <= 0 || height <= 0)
        return;

    vgfx_window_t window = (vgfx_window_t)canvas;

    // Extract color components
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    // Get framebuffer for direct pixel access with alpha blending
    vgfx_framebuffer_t fb;
    if (!vgfx_get_framebuffer(window, &fb)) {
        return; // vgfx_get_framebuffer returns 1 on success, 0 on failure
    }

    int canvas_w = fb.width;
    int canvas_h = fb.height;
    int clip_x = 0;
    int clip_y = 0;
    int clip_w = canvas_w;
    int clip_h = canvas_h;
    (void)vgfx_get_clip(window, &clip_x, &clip_y, &clip_w, &clip_h);

    int start_x = x;
    int start_y = y;
    int end_x = x + width;
    int end_y = y + height;

    if (start_x < clip_x)
        start_x = clip_x;
    if (start_y < clip_y)
        start_y = clip_y;
    if (end_x > clip_x + clip_w)
        end_x = clip_x + clip_w;
    if (end_y > clip_y + clip_h)
        end_y = clip_y + clip_h;
    if (start_x < 0)
        start_x = 0;
    if (start_y < 0)
        start_y = 0;
    if (end_x > canvas_w)
        end_x = canvas_w;
    if (end_y > canvas_h)
        end_y = canvas_h;
    if (start_x >= end_x || start_y >= end_y)
        return;

    // Draw each pixel with alpha blending
    // Use byte-level access matching vgfx's RGBA format
    for (int screen_y = start_y; screen_y < end_y; screen_y++) {
        int py = screen_y - y;
        for (int screen_x = start_x; screen_x < end_x; screen_x++) {
            int px = screen_x - x;
            uint8_t alpha = bitmap[py * width + px];
            if (alpha == 0)
                continue;

            // Calculate pixel address (RGBA format, 4 bytes per pixel)
            uint8_t *pixel = fb.pixels + (screen_y * fb.stride) + (screen_x * 4);

            if (alpha == 255) {
                // Fully opaque - just write the color (RGBA order)
                pixel[0] = r;
                pixel[1] = g;
                pixel[2] = b;
                pixel[3] = 0xFF;
            } else if (vg_text_gamma_blend_enabled()) {
                // Gamma-correct AA: blend coverage in linear light via the
                // checked-in integer tables, then re-encode to sRGB. This
                // stops light-on-dark text from over-darkening at glyph edges
                // (Zanna Studio plan 06).
                uint32_t inv_alpha = 255 - alpha;
                uint32_t lin_fg_r = k_vg_srgb_to_linear[r];
                uint32_t lin_fg_g = k_vg_srgb_to_linear[g];
                uint32_t lin_fg_b = k_vg_srgb_to_linear[b];
                uint32_t lin_bg_r = k_vg_srgb_to_linear[pixel[0]];
                uint32_t lin_bg_g = k_vg_srgb_to_linear[pixel[1]];
                uint32_t lin_bg_b = k_vg_srgb_to_linear[pixel[2]];
                uint32_t out_r = (lin_fg_r * alpha + lin_bg_r * inv_alpha + 127) / 255;
                uint32_t out_g = (lin_fg_g * alpha + lin_bg_g * inv_alpha + 127) / 255;
                uint32_t out_b = (lin_fg_b * alpha + lin_bg_b * inv_alpha + 127) / 255;
                pixel[0] = k_vg_linear_to_srgb[(out_r > 32767 ? 32767 : out_r) >> 3];
                pixel[1] = k_vg_linear_to_srgb[(out_g > 32767 ? 32767 : out_g) >> 3];
                pixel[2] = k_vg_linear_to_srgb[(out_b > 32767 ? 32767 : out_b) >> 3];
                pixel[3] = 0xFF;
            } else {
                // Legacy sRGB-space blend (ZANNA_GUI_TEXT_GAMMA=off).
                uint8_t bg_r = pixel[0];
                uint8_t bg_g = pixel[1];
                uint8_t bg_b = pixel[2];

                // Fast alpha blending
                uint32_t inv_alpha = 255 - alpha;
                pixel[0] = (uint8_t)((r * alpha + bg_r * inv_alpha + 127) / 255);
                pixel[1] = (uint8_t)((g * alpha + bg_g * inv_alpha + 127) / 255);
                pixel[2] = (uint8_t)((b * alpha + bg_b * inv_alpha + 127) / 255);
                pixel[3] = 0xFF;
            }
        }
    }
}

//=============================================================================
// Alternative: Draw using vgfx_pset
//=============================================================================

/// @brief Draw a glyph bitmap via vgfx_pset — slower fallback that does not
///        require direct framebuffer access.
///
/// @details Uses a simple alpha-threshold (>= 128 = draw) instead of full
///          per-pixel blending. Suitable for contexts where the framebuffer
///          pointer is unavailable.
///
/// @param canvas  vgfx_window_t handle to draw into.
/// @param x       Left edge of the glyph in canvas coordinates.
/// @param y       Top edge of the glyph in canvas coordinates.
/// @param bitmap  8-bit alpha coverage bitmap (width × height bytes).
/// @param width   Width of the bitmap in pixels.
/// @param height  Height of the bitmap in pixels.
/// @param color   Glyph foreground colour in 0xRRGGBB format.
void vg_canvas_draw_glyph_pset(
    void *canvas, int x, int y, const uint8_t *bitmap, int width, int height, uint32_t color) {
    if (!canvas || !bitmap || width <= 0 || height <= 0)
        return;

    vgfx_window_t window = (vgfx_window_t)canvas;

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            uint8_t alpha = bitmap[py * width + px];
            if (alpha == 0)
                continue;

            int screen_x = x + px;
            int screen_y = y + py;

            if (alpha >= 128) {
                // Simple threshold for non-framebuffer version
                vgfx_pset(window, screen_x, screen_y, vgfx_rgb(r, g, b));
            }
        }
    }
}
