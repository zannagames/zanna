//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
/// @file rt_graphics.h
/// @brief Declares the public C ABI for Canvas2D graphics and window management.
///
/// @details
/// The API exposes opaque canvas handles, logical-pixel drawing primitives,
/// framebuffer operations, color helpers, event pumping, timing, and window
/// state. Graphics-disabled builds retain the same symbols through dedicated
/// fallback stubs.
///
// File: src/runtime/graphics/rt_graphics.h
// Purpose: Runtime bridge functions for the ZannaGFX graphics library, providing canvas
// creation/destruction, drawing operations, pixel manipulation, image loading, and window
// management.
//
// Key invariants:
//   - All canvas pointers are opaque handles returned by rt_canvas_new.
//   - Drawing coordinates are in logical pixels; HiDPI scaling is applied internally.
//   - rt_canvas_poll pumps OS events and updates input state for the frame.
//   - rt_canvas_flip presents the completed frame to the display.
//
// Ownership/Lifetime:
//   - Canvas handles must be destroyed with rt_canvas_destroy when no longer needed.
//   - Graphics system resources are cleaned up on destruction.
//
// Links: src/runtime/graphics/rt_graphics.c (implementation), src/lib/graphics/include/vgfx.h,
// src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_CANVAS_CLASS_ID INT64_C(-0x520001)

//=========================================================================
// Canvas Functions
//=========================================================================

/// @brief Report whether graphics support is compiled into this runtime.
/// @return 1 when ZannaGFX-backed Canvas support is available, 0 otherwise.
int8_t rt_canvas_is_available(void);

/// @brief Create a new graphics canvas.
/// @param title Window title (runtime string).
/// @param width Canvas width in pixels.
/// @param height Canvas height in pixels.
/// @return Opaque canvas handle, or NULL on failure.
void *rt_canvas_new(rt_string title, int64_t width, int64_t height);

/// @brief Return 1 if @p canvas is a live Canvas handle, 0 otherwise.
/// @param canvas Candidate opaque handle.
/// @return `1` for a live Canvas handle; otherwise `0`.
int8_t rt_canvas_is_handle(void *canvas);
/// @brief Opaque platform-window forward declaration (full type in vgfx.h).
struct vgfx_window;
/// @brief Exclusively borrow and retain the platform window behind a live 2D canvas.
/// @details At most one borrower may hold the window. A successful call retains the Canvas until
/// `rt_canvas_return_window` releases the loan.
/// @param canvas Candidate Canvas handle.
/// @return Borrowed window, or NULL for an invalid, closed, or already-loaned Canvas.
struct vgfx_window *rt_canvas_borrow_window(void *canvas);
/// @brief Return an exclusive platform-window loan and release its retained Canvas owner.
/// @param canvas Canvas passed to a successful `rt_canvas_borrow_window`; invalid or duplicate
/// returns are ignored.
void rt_canvas_return_window(void *canvas);
/// @brief Invalidate a 2D canvas's cached window state (post-3D-adoption resync).
/// @param canvas Candidate Canvas handle; invalid handles are ignored.
void rt_canvas_mark_window_state_dirty(void *canvas);

/// @brief Destroy a graphics canvas and free resources.
/// @param canvas Canvas handle from rt_canvas_new.
void rt_canvas_destroy(void *canvas);

/// @brief Get the canvas width.
/// @param canvas Canvas handle.
/// @return Canvas width in pixels.
int64_t rt_canvas_width(void *canvas);

/// @brief Get the canvas height.
/// @param canvas Canvas handle.
/// @return Canvas height in pixels.
int64_t rt_canvas_height(void *canvas);

/// @brief Check if canvas should close.
/// @param canvas Canvas handle.
/// @return 1 if close was requested, 0 otherwise.
int64_t rt_canvas_should_close(void *canvas);

/// @brief Swap buffers and display drawn content.
/// @param canvas Canvas handle.
void rt_canvas_flip(void *canvas);

/// @brief Clear the canvas to a solid color.
/// @param canvas Canvas handle.
/// @param color RGB color (0x00RRGGBB).
void rt_canvas_clear(void *canvas, int64_t color);

/// @brief Draw a line from (x1,y1) to (x2,y2).
/// @param canvas Canvas handle.
/// @param x1 Starting X coordinate.
/// @param y1 Starting Y coordinate.
/// @param x2 Ending X coordinate.
/// @param y2 Ending Y coordinate.
/// @param color Line color (0x00RRGGBB).
void rt_canvas_line(void *canvas, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t color);

/// @brief Draw a filled rectangle.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_box(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);

/// @brief Draw a rectangle outline.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_frame(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color);

/// @brief Draw a filled circle.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param radius Circle radius in pixels.
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_disc(void *canvas, int64_t cx, int64_t cy, int64_t radius, int64_t color);

/// @brief Draw a circle outline.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param radius Circle radius in pixels.
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_ring(void *canvas, int64_t cx, int64_t cy, int64_t radius, int64_t color);

/// @brief Set a single pixel to a color.
/// @param canvas Canvas handle.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @param color Pixel color (0x00RRGGBB).
void rt_canvas_plot(void *canvas, int64_t x, int64_t y, int64_t color);

/// @brief Pump pending OS events and update per-frame input state.
/// @details Processes pending window/input events, forwards text and scroll
///   input to the runtime input layer, and then updates action bindings.
/// @param canvas Canvas handle.
/// @return Event type code (0 = no event).
int64_t rt_canvas_poll(void *canvas);

/// @brief Check if a key is currently held down.
/// @param canvas Canvas handle.
/// @param key Key code to check.
/// @return 1 if pressed, 0 otherwise.
int64_t rt_canvas_key_held(void *canvas, int64_t key);

/// @brief Draw text at the specified position.
/// @param canvas Canvas handle.
/// @param x X coordinate (left edge).
/// @param y Y coordinate (top edge).
/// @param text Text string to draw.
/// @param color Text color (0x00RRGGBB).
void rt_canvas_text(void *canvas, int64_t x, int64_t y, rt_string text, int64_t color);

/// @brief Draw text with foreground and background colors.
/// @param canvas Canvas handle.
/// @param x X coordinate (left edge).
/// @param y Y coordinate (top edge).
/// @param text Text string to draw.
/// @param fg Foreground (text) color (0x00RRGGBB).
/// @param bg Background color (0x00RRGGBB).
void rt_canvas_text_bg(void *canvas, int64_t x, int64_t y, rt_string text, int64_t fg, int64_t bg);

/// @brief Get the width of rendered text in pixels.
/// @param text Text string to measure.
/// @return Width in pixels (8 pixels per decoded codepoint; unsupported
///         built-in glyphs still consume one character cell).
int64_t rt_canvas_text_width(rt_string text);

/// @brief Get the height of rendered text in pixels.
/// @return Height in pixels (always 8 for the built-in font).
int64_t rt_canvas_text_height(void);

/// @brief Draw text at an integer scale factor.
/// @param canvas Canvas handle.
/// @param x X coordinate (left edge).
/// @param y Y coordinate (top edge).
/// @param text Text string to draw.
/// @param scale Integer scale factor (1=8px, 2=16px, 3=24px, etc.).
/// @param color Text color (0x00RRGGBB).
void rt_canvas_text_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t color);

/// @brief Draw scaled text with foreground and background colors.
/// @param canvas Canvas handle.
/// @param x X coordinate (left edge).
/// @param y Y coordinate (top edge).
/// @param text Text string to draw.
/// @param scale Integer scale factor.
/// @param fg Foreground (text) color (0x00RRGGBB).
/// @param bg Background color (0x00RRGGBB).
void rt_canvas_text_scaled_bg(
    void *canvas, int64_t x, int64_t y, rt_string text, int64_t scale, int64_t fg, int64_t bg);

/// @brief Get the width of scaled text in pixels.
/// @param text Text string to measure.
/// @param scale Integer scale factor.
/// @return Width in pixels (8 * scale per decoded codepoint).
int64_t rt_canvas_text_scaled_width(rt_string text, int64_t scale);

/// @brief Draw text horizontally centered on the canvas.
/// @param canvas Canvas handle.
/// @param y Top-edge Y coordinate.
/// @param text Text to draw.
/// @param color Packed text color.
void rt_canvas_text_centered(void *canvas, int64_t y, rt_string text, int64_t color);

/// @brief Draw text right-aligned with a margin from the right edge.
/// @param canvas Canvas handle.
/// @param margin Distance from the right canvas edge in logical pixels.
/// @param y Top-edge Y coordinate.
/// @param text Text to draw.
/// @param color Packed text color.
void rt_canvas_text_right(void *canvas, int64_t margin, int64_t y, rt_string text, int64_t color);

/// @brief Draw scaled text horizontally centered on the canvas.
/// @param canvas Canvas handle.
/// @param y Top-edge Y coordinate.
/// @param text Text to draw.
/// @param color Packed text color.
/// @param scale Positive integer glyph scale.
void rt_canvas_text_centered_scaled(
    void *canvas, int64_t y, rt_string text, int64_t color, int64_t scale);

/// @brief Draw a filled rectangle with alpha blending.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Fill color (0x00RRGGBB).
/// @param alpha Alpha value (0=transparent, 255=opaque).
void rt_canvas_box_alpha(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t color, int64_t alpha);

/// @brief Draw a filled circle with alpha blending.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param radius Circle radius in pixels.
/// @param color Fill color (0x00RRGGBB).
/// @param alpha Alpha value (0=transparent, 255=opaque).
void rt_canvas_disc_alpha(
    void *canvas, int64_t cx, int64_t cy, int64_t radius, int64_t color, int64_t alpha);

/// @brief Draw a filled ellipse with alpha blending.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param rx Horizontal radius.
/// @param ry Vertical radius.
/// @param color Fill color (0x00RRGGBB).
/// @param alpha Alpha value (0=transparent, 255=opaque).
void rt_canvas_ellipse_alpha(
    void *canvas, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color, int64_t alpha);

/// @brief Blit a Pixels buffer to the canvas.
/// @param canvas Canvas handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param pixels Source Pixels object.
/// @note Uses logical canvas coordinates and honors the active clip rectangle.
void rt_canvas_blit(void *canvas, int64_t x, int64_t y, void *pixels);

/// @brief Blit a region of a Pixels buffer to the canvas.
/// @param canvas Canvas handle.
/// @param dx Destination X coordinate.
/// @param dy Destination Y coordinate.
/// @param pixels Source Pixels object.
/// @param sx Source X coordinate.
/// @param sy Source Y coordinate.
/// @param w Width of region to blit.
/// @param h Height of region to blit.
/// @note Uses logical canvas coordinates and honors the active clip rectangle.
void rt_canvas_blit_region(void *canvas,
                           int64_t dx,
                           int64_t dy,
                           void *pixels,
                           int64_t sx,
                           int64_t sy,
                           int64_t w,
                           int64_t h);

/// @brief Blit a Pixels buffer with alpha blending.
/// @param canvas Canvas handle.
/// @param x Destination X coordinate.
/// @param y Destination Y coordinate.
/// @param pixels Source Pixels object (RGBA format).
/// @note Uses logical canvas coordinates and honors the active clip rectangle.
void rt_canvas_blit_alpha(void *canvas, int64_t x, int64_t y, void *pixels);

//=========================================================================
// Extended Drawing Primitives
//=========================================================================

/// @brief Draw a thick line from (x1,y1) to (x2,y2).
/// @param canvas Canvas handle.
/// @param x1 Starting X coordinate.
/// @param y1 Starting Y coordinate.
/// @param x2 Ending X coordinate.
/// @param y2 Ending Y coordinate.
/// @param thickness Line thickness in pixels.
/// @param color Line color (0x00RRGGBB).
void rt_canvas_thick_line(
    void *canvas, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t thickness, int64_t color);

/// @brief Draw a filled rounded rectangle.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param radius Corner radius in pixels.
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_round_box(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color);

/// @brief Draw a rounded rectangle outline.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param radius Corner radius in pixels.
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_round_frame(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t radius, int64_t color);

/// @brief Flood fill an area starting from (x,y).
/// @param canvas Canvas handle.
/// @param x Starting X coordinate.
/// @param y Starting Y coordinate.
/// @param color Fill color (0x00RRGGBB).
/// @note Fills connected pixels of the same color as the starting pixel and
///       does not expand beyond the active clip rectangle.
void rt_canvas_flood_fill(void *canvas, int64_t x, int64_t y, int64_t color);

/// @brief Draw a filled triangle.
/// @param canvas Canvas handle.
/// @param x1 First vertex X.
/// @param y1 First vertex Y.
/// @param x2 Second vertex X.
/// @param y2 Second vertex Y.
/// @param x3 Third vertex X.
/// @param y3 Third vertex Y.
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_triangle(void *canvas,
                        int64_t x1,
                        int64_t y1,
                        int64_t x2,
                        int64_t y2,
                        int64_t x3,
                        int64_t y3,
                        int64_t color);

/// @brief Draw a triangle outline.
/// @param canvas Canvas handle.
/// @param x1 First vertex X.
/// @param y1 First vertex Y.
/// @param x2 Second vertex X.
/// @param y2 Second vertex Y.
/// @param x3 Third vertex X.
/// @param y3 Third vertex Y.
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_triangle_frame(void *canvas,
                              int64_t x1,
                              int64_t y1,
                              int64_t x2,
                              int64_t y2,
                              int64_t x3,
                              int64_t y3,
                              int64_t color);

/// @brief Draw a filled ellipse.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param rx Horizontal radius.
/// @param ry Vertical radius.
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_ellipse(void *canvas, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color);

/// @brief Draw an ellipse outline.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param rx Horizontal radius.
/// @param ry Vertical radius.
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_ellipse_frame(
    void *canvas, int64_t cx, int64_t cy, int64_t rx, int64_t ry, int64_t color);

//=========================================================================
// Phase 4: Advanced Curves & Shapes
//=========================================================================

/// @brief Draw a filled arc (pie slice).
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param radius Arc radius.
/// @param start_angle Starting angle in degrees (0 = right, 90 = up).
/// @param end_angle Ending angle in degrees.
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_arc(void *canvas,
                   int64_t cx,
                   int64_t cy,
                   int64_t radius,
                   int64_t start_angle,
                   int64_t end_angle,
                   int64_t color);

/// @brief Draw an arc outline.
/// @param canvas Canvas handle.
/// @param cx Center X coordinate.
/// @param cy Center Y coordinate.
/// @param radius Arc radius.
/// @param start_angle Starting angle in degrees (0 = right, 90 = up).
/// @param end_angle Ending angle in degrees.
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_arc_frame(void *canvas,
                         int64_t cx,
                         int64_t cy,
                         int64_t radius,
                         int64_t start_angle,
                         int64_t end_angle,
                         int64_t color);

/// @brief Draw a quadratic Bezier curve.
/// @param canvas Canvas handle.
/// @param x1 Start point X.
/// @param y1 Start point Y.
/// @param cx Control point X.
/// @param cy Control point Y.
/// @param x2 End point X.
/// @param y2 End point Y.
/// @param color Line color (0x00RRGGBB).
void rt_canvas_bezier(void *canvas,
                      int64_t x1,
                      int64_t y1,
                      int64_t cx,
                      int64_t cy,
                      int64_t x2,
                      int64_t y2,
                      int64_t color);

/// @brief Draw connected line segments (polyline).
/// @param canvas Canvas handle.
/// @param points Array of point coordinates [x1, y1, x2, y2, ...].
/// @param count Number of points (pairs of coordinates).
/// @param color Line color (0x00RRGGBB).
void rt_canvas_polyline(void *canvas, void *points, int64_t count, int64_t color);

/// @brief Draw a filled polygon.
/// @param canvas Canvas handle.
/// @param points Array of point coordinates [x1, y1, x2, y2, ...].
/// @param count Number of points (pairs of coordinates).
/// @param color Fill color (0x00RRGGBB).
void rt_canvas_polygon(void *canvas, void *points, int64_t count, int64_t color);

/// @brief Draw a polygon outline.
/// @param canvas Canvas handle.
/// @param points Array of point coordinates [x1, y1, x2, y2, ...].
/// @param count Number of points (pairs of coordinates).
/// @param color Outline color (0x00RRGGBB).
void rt_canvas_polygon_frame(void *canvas, void *points, int64_t count, int64_t color);

/// @brief Draw connected line segments from a typed Path2D.
/// @param canvas Canvas handle.
/// @param path Typed Path2D handle containing the vertices.
/// @param color Packed line color.
void rt_canvas_polyline_path(void *canvas, void *path, int64_t color);

/// @brief Draw a filled polygon from a typed Path2D.
/// @param canvas Canvas handle.
/// @param path Typed Path2D handle containing the closed polygon vertices.
/// @param color Packed fill color.
void rt_canvas_polygon_path(void *canvas, void *path, int64_t color);

/// @brief Draw a polygon outline from a typed Path2D.
/// @param canvas Canvas handle.
/// @param path Typed Path2D handle containing the closed polygon vertices.
/// @param color Packed outline color.
void rt_canvas_polygon_frame_path(void *canvas, void *path, int64_t color);

//=========================================================================
// Phase 5: Canvas Utilities
//=========================================================================

/// @brief Get a pixel color from the canvas.
/// @param canvas Canvas handle.
/// @param x X coordinate.
/// @param y Y coordinate.
/// @return Pixel color (0x00RRGGBB), or 0 if out of bounds.
int64_t rt_canvas_get_pixel(void *canvas, int64_t x, int64_t y);

/// @brief Copy a rectangular region from canvas to a Pixels buffer.
/// @param canvas Canvas handle.
/// @param x Source X coordinate.
/// @param y Source Y coordinate.
/// @param w Width of region.
/// @param h Height of region.
/// @return New Pixels object containing the copied region, or NULL on failure.
void *rt_canvas_copy_rect(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Save the canvas contents to a BMP file.
/// @param canvas Canvas handle.
/// @param path File path to save to.
/// @return 1 on success, 0 on failure.
int64_t rt_canvas_save_bmp(void *canvas, rt_string path);

/// @brief Save the canvas contents to a PNG file.
/// @param canvas Canvas handle.
/// @param path File path to save to.
/// @return 1 on success, 0 on failure.
int64_t rt_canvas_save_png(void *canvas, rt_string path);

//=========================================================================
// Color Functions
//=========================================================================

/// @brief Return the packed red color constant.
/// @return Packed `0x00FF0000`.
int64_t rt_color_red(void);
/// @brief Return the packed green color constant.
/// @return Packed `0x0000FF00`.
int64_t rt_color_green(void);
/// @brief Return the packed blue color constant.
/// @return Packed `0x000000FF`.
int64_t rt_color_blue(void);
/// @brief Return the packed white color constant.
/// @return Packed `0x00FFFFFF`.
int64_t rt_color_white(void);
/// @brief Return the packed black color constant.
/// @return Packed `0x00000000`.
int64_t rt_color_black(void);
/// @brief Return the packed yellow color constant.
/// @return Packed `0x00FFFF00`.
int64_t rt_color_yellow(void);
/// @brief Return the packed cyan color constant.
/// @return Packed `0x0000FFFF`.
int64_t rt_color_cyan(void);
/// @brief Return the packed magenta color constant.
/// @return Packed `0x00FF00FF`.
int64_t rt_color_magenta(void);
/// @brief Return the packed gray color constant.
/// @return The runtime's packed gray constant.
int64_t rt_color_gray(void);
/// @brief Return the packed orange color constant.
/// @return The runtime's packed orange constant.
int64_t rt_color_orange(void);

/// @brief Construct a packed color from RGB channel values.
/// @param r Red component (0-255).
/// @param g Green component (0-255).
/// @param b Blue component (0-255).
/// @return Packed color value (0x00RRGGBB).
int64_t rt_color_rgb(int64_t r, int64_t g, int64_t b);

/// @brief Construct a color from RGBA components.
/// @param r Red component (0-255).
/// @param g Green component (0-255).
/// @param b Blue component (0-255).
/// @param a Alpha component (0-255).
/// @return Packed color value (0xAARRGGBB).
int64_t rt_color_rgba(int64_t r, int64_t g, int64_t b, int64_t a);

/// @brief Construct a color from HSL components.
/// @param h Hue (0-360).
/// @param s Saturation (0-100).
/// @param l Lightness (0-100).
/// @return Packed color value (0x00RRGGBB).
int64_t rt_color_from_hsl(int64_t h, int64_t s, int64_t l);

/// @brief Extract hue from a color.
/// @param color Packed color value (0x00RRGGBB).
/// @return Hue (0-360).
int64_t rt_color_get_h(int64_t color);

/// @brief Extract saturation from a color.
/// @param color Packed color value (0x00RRGGBB).
/// @return Saturation (0-100).
int64_t rt_color_get_s(int64_t color);

/// @brief Extract lightness from a color.
/// @param color Packed color value (0x00RRGGBB).
/// @return Lightness (0-100).
int64_t rt_color_get_l(int64_t color);

/// @brief Linearly interpolate between two colors.
/// @param c1 First color (0x00RRGGBB).
/// @param c2 Second color (0x00RRGGBB).
/// @param t Interpolation factor (0-100, where 0=c1, 100=c2).
/// @return Interpolated color.
int64_t rt_color_lerp(int64_t c1, int64_t c2, int64_t t);

/// @brief Extract red component from a color.
/// @param color Packed color value (0x00RRGGBB or 0xAARRGGBB).
/// @return Red component (0-255).
int64_t rt_color_get_r(int64_t color);

/// @brief Extract green component from a color.
/// @param color Packed color value (0x00RRGGBB or 0xAARRGGBB).
/// @return Green component (0-255).
int64_t rt_color_get_g(int64_t color);

/// @brief Extract blue component from a color.
/// @param color Packed color value (0x00RRGGBB or 0xAARRGGBB).
/// @return Blue component (0-255).
int64_t rt_color_get_b(int64_t color);

/// @brief Extract alpha component from a color.
/// @param color Packed color value (0xAARRGGBB).
/// @return Alpha component (0-255).
int64_t rt_color_get_a(int64_t color);

/// @brief Brighten a color.
/// @param color Packed color value (0x00RRGGBB).
/// @param amount Amount to brighten (0-100).
/// @return Brightened color.
int64_t rt_color_brighten(int64_t color, int64_t amount);

/// @brief Darken a color.
/// @param color Packed color value (0x00RRGGBB).
/// @param amount Amount to darken (0-100).
/// @return Darkened color.
int64_t rt_color_darken(int64_t color, int64_t amount);

/// @brief Parse a hex color string (e.g., "#FF0000" or "#FF000080").
/// @param hex Hex string with or without '#' prefix.
/// @return Packed color value (0xAARRGGBB).
int64_t rt_color_from_hex(rt_string hex);

/// @brief Convert a color to hex string format.
/// @param color Packed color value.
/// @return Hex string like "#RRGGBB" or "#RRGGBBAA" if alpha != 255.
rt_string rt_color_to_hex(int64_t color);

/// @brief Increase saturation of a color.
/// @param color Packed color value.
/// @param amount Amount to increase (0-100).
/// @return Saturated color.
int64_t rt_color_saturate(int64_t color, int64_t amount);

/// @brief Decrease saturation of a color.
/// @param color Packed color value.
/// @param amount Amount to decrease (0-100).
/// @return Desaturated color.
int64_t rt_color_desaturate(int64_t color, int64_t amount);

/// @brief Get the complementary color (opposite on color wheel).
/// @param color Packed color value.
/// @return Complementary color.
int64_t rt_color_complement(int64_t color);

/// @brief Convert a color to grayscale.
/// @param color Packed color value.
/// @return Grayscale color.
int64_t rt_color_grayscale(int64_t color);

/// @brief Invert a color (255 - each channel).
/// @param color Packed color value.
/// @return Inverted color.
int64_t rt_color_invert(int64_t color);

//=========================================================================
// Canvas Extended Functions
//=========================================================================

/// @brief Set a clipping rectangle for drawing operations.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Width of clipping region.
/// @param h Height of clipping region.
void rt_canvas_set_clip_rect(void *canvas, int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Clear the clipping rectangle (restore full canvas drawing).
/// @param canvas Canvas handle.
void rt_canvas_clear_clip_rect(void *canvas);

/// @brief Set the window title.
/// @param canvas Canvas handle.
/// @param title New window title.
void rt_canvas_set_title(void *canvas, rt_string title);

/// @brief Get the current window title.
/// @param canvas Canvas handle.
/// @return Window title string.
rt_string rt_canvas_get_title(void *canvas);

/// @brief Resize the canvas window.
/// @param canvas Canvas handle.
/// @param width New width in pixels.
/// @param height New height in pixels.
void rt_canvas_resize(void *canvas, int64_t width, int64_t height);

/// @brief Close and destroy the canvas window.
/// @param canvas Canvas handle.
void rt_canvas_close(void *canvas);

/// @brief Capture the canvas contents to a Pixels buffer.
/// @param canvas Canvas handle.
/// @return New Pixels object containing the canvas contents.
void *rt_canvas_screenshot(void *canvas);

/// @brief Enter fullscreen mode.
/// @param canvas Canvas handle.
void rt_canvas_fullscreen(void *canvas);

/// @brief Exit fullscreen mode (return to windowed).
/// @param canvas Canvas handle.
void rt_canvas_windowed(void *canvas);

/// @brief Draw a horizontal gradient.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Width.
/// @param h Height.
/// @param c1 Left color (0x00RRGGBB).
/// @param c2 Right color (0x00RRGGBB).
/// @note Honors the active clip rectangle.
void rt_canvas_gradient_h(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c1, int64_t c2);

/// @brief Draw a vertical gradient.
/// @param canvas Canvas handle.
/// @param x Left edge X coordinate.
/// @param y Top edge Y coordinate.
/// @param w Width.
/// @param h Height.
/// @param c1 Top color (0x00RRGGBB).
/// @param c2 Bottom color (0x00RRGGBB).
/// @note Honors the active clip rectangle.
void rt_canvas_gradient_v(
    void *canvas, int64_t x, int64_t y, int64_t w, int64_t h, int64_t c1, int64_t c2);

// --- Window management (BINDING-001 + BINDING-002) ---

/// @brief Get the display scale factor (1.0 on standard, 2.0 on HiDPI/Retina).
/// @param canvas Canvas handle.
/// @return Ratio of physical framebuffer pixels to logical canvas pixels.
double rt_canvas_get_scale(void *canvas);

/// @brief Get the window X position in screen coordinates.
/// @param canvas Canvas handle.
/// @return Window top-left X coordinate in screen pixels.
int64_t rt_canvas_get_window_x(void *canvas);

/// @brief Get the window Y position in screen coordinates.
/// @param canvas Canvas handle.
/// @return Window top-left Y coordinate in screen pixels.
int64_t rt_canvas_get_window_y(void *canvas);

/// @brief Get the window position in screen coordinates.
/// @param canvas Canvas handle.
/// @param out_x Receives X coordinate.
/// @param out_y Receives Y coordinate.
void rt_canvas_get_position(void *canvas, int64_t *out_x, int64_t *out_y);

/// @brief Set the window position in screen coordinates.
/// @param canvas Canvas handle.
/// @param x New top-left X coordinate.
/// @param y New top-left Y coordinate.
void rt_canvas_set_position(void *canvas, int64_t x, int64_t y);

/// @brief Get the current target FPS (-1 = unlimited).
/// @param canvas Canvas handle.
/// @return Target frames per second, or `-1` when unlimited.
int64_t rt_canvas_get_fps(void *canvas);

/// @brief Set the target FPS (<=0 = unlimited).
/// @param canvas Canvas handle.
/// @param fps Target frames per second; non-positive disables the cap.
void rt_canvas_set_fps(void *canvas, int64_t fps);

/// @brief Get milliseconds elapsed since the last Flip() call.
/// If SetDTMax was called, result is clamped to [1, max] after startup while
/// preserving an exact 0 ms first frame.
/// @param canvas Canvas handle.
/// @return Elapsed frame time in milliseconds.
int64_t rt_canvas_get_delta_time(void *canvas);

/// @brief Get seconds elapsed since the last Flip() call.
/// Uses the same clamp as rt_canvas_get_delta_time().
/// @param canvas Canvas handle.
/// @return Elapsed frame time in seconds.
double rt_canvas_get_delta_time_sec(void *canvas);

/// @brief Set the maximum delta time clamp.
/// When set, DeltaTime is clamped to [1, max] after the first frame. Pass 0 to
/// disable clamping (default).
/// @param canvas Canvas handle.
/// @param max_ms Maximum reported frame interval in milliseconds; `0` disables clamping.
void rt_canvas_set_dt_max(void *canvas, int64_t max_ms);

/// @brief Poll events and check if the window should remain open.
/// Equivalent to calling Poll() then checking !ShouldClose.
/// @param canvas Canvas handle.
/// @return 1 if the frame should proceed, 0 if the window is closing.
int64_t rt_canvas_begin_frame(void *canvas);

/// @brief Return 1 if the window is maximized, 0 otherwise.
/// @param canvas Canvas handle.
/// @return `1` when maximized; otherwise `0`.
int8_t rt_canvas_is_maximized(void *canvas);

/// @brief Maximize the window.
/// @param canvas Canvas handle.
void rt_canvas_maximize(void *canvas);

/// @brief Return 1 if the window is minimized, 0 otherwise.
/// @param canvas Canvas handle.
/// @return `1` when minimized; otherwise `0`.
int8_t rt_canvas_is_minimized(void *canvas);

/// @brief Minimize (iconify) the window.
/// @param canvas Canvas handle.
void rt_canvas_minimize(void *canvas);

/// @brief Restore from minimized or maximized state.
/// @param canvas Canvas handle.
void rt_canvas_restore(void *canvas);

/// @brief Return 1 if the window has keyboard focus, 0 otherwise.
/// @param canvas Canvas handle.
/// @return `1` when focused; otherwise `0`.
int8_t rt_canvas_is_focused(void *canvas);

/// @brief Bring the window to the front and give it focus.
/// @param canvas Canvas handle.
void rt_canvas_focus(void *canvas);

/// @brief Allow (0) or prevent (1) the window close button.
/// @param canvas Canvas handle.
/// @param prevent Non-zero to suppress close-button requests.
void rt_canvas_prevent_close(void *canvas, int64_t prevent);

/// @brief Get the monitor size in pixels.
/// @param canvas Canvas handle.
/// @param out_w Output receiving the monitor width.
/// @param out_h Output receiving the monitor height.
void rt_canvas_get_monitor_size(void *canvas, int64_t *out_w, int64_t *out_h);

/// @brief Get the monitor width in pixels.
/// @param canvas Canvas handle.
/// @return Monitor width in physical pixels.
int64_t rt_canvas_get_monitor_width(void *canvas);

/// @brief Get the monitor height in pixels.
/// @param canvas Canvas handle.
/// @return Monitor height in physical pixels.
int64_t rt_canvas_get_monitor_height(void *canvas);

// Additive and float-channel operations (ADR 0253).
/// @brief Offset every RGB channel by a signed amount, clamped to 0..255.
/// @details Additive, unlike the percentage-based Brighten/Darken pair.
/// @param color Runtime color to transform.
/// @param amount Signed per-channel offset.
/// @return Offset color.
int64_t rt_color_add_channels(int64_t color, int64_t amount);
/// @brief Scale every RGB channel by a factor, clamped to 0..255.
/// @param color Runtime color to transform.
/// @param factor Multiplier; negative or non-finite becomes zero.
/// @return Scaled color.
int64_t rt_color_scale_rgb(int64_t color, double factor);
/// @brief Pack three 0..1 float channels into a runtime color.
/// @param r Red in 0..1.
/// @param g Green in 0..1.
/// @param b Blue in 0..1.
/// @return Packed 0xRRGGBB color.
int64_t rt_color_rgb_f(double r, double g, double b);
/// @brief Red channel as a 0..1 float.
/// @param color Runtime color.
/// @return Red divided by 255.
double rt_color_get_red_f(int64_t color);
/// @brief Green channel as a 0..1 float.
/// @param color Runtime color.
/// @return Green divided by 255.
double rt_color_get_green_f(int64_t color);
/// @brief Blue channel as a 0..1 float.
/// @param color Runtime color.
/// @return Blue divided by 255.
double rt_color_get_blue_f(int64_t color);
/// @brief Rec.709 relative luminance of a color, 0..1.
/// @param color Runtime color.
/// @return Luminance in 0..1.
double rt_color_luma(int64_t color);

#ifdef __cplusplus
}
#endif
