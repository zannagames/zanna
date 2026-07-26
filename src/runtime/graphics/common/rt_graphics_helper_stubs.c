//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file rt_graphics_helper_stubs.c
/// @brief Graphics-disabled color, transform, and utility helper stubs.
///
/// @details This split source groups backend-independent graphics helpers and
/// unavailable-backend utility entry points without changing their exported C
/// names or fallback semantics.
///
// File: src/runtime/graphics/common/rt_graphics_helper_stubs.c
// Purpose: Graphics-disabled pure helper entry points that remain backend independent.
//
// Key invariants:
//   - Compiled only for graphics-disabled runtime builds.
//   - Stateful graphics APIs fail with the shared InvalidOperation trap.
//   - Backend-independent query helpers keep their documented fallback values.
//
// Ownership/Lifetime:
//   - Stub entry points allocate no graphics resources and retain no handles.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"
#include "rt_graphics_internal.h"

#ifndef RT_COLOR_EXPLICIT_ALPHA_FLAG
#define RT_COLOR_EXPLICIT_ALPHA_FLAG ((int64_t)1 << 56)
#endif

// Color constants — packed 0x00RRGGBB
/// @brief Return the predefined red color constant.
/// @return Packed `0x00FF0000`.
int64_t rt_color_red(void) {
    return 0xFF0000;
}

/// @brief Return the predefined green color constant.
/// @return Packed `0x0000FF00`.
int64_t rt_color_green(void) {
    return 0x00FF00;
}

/// @brief Return the predefined blue color constant.
/// @return Packed `0x000000FF`.
int64_t rt_color_blue(void) {
    return 0x0000FF;
}

/// @brief Return the predefined white color constant.
/// @return Packed `0x00FFFFFF`.
int64_t rt_color_white(void) {
    return 0xFFFFFF;
}

/// @brief Return the predefined black color constant.
/// @return Packed `0x00000000`.
int64_t rt_color_black(void) {
    return 0x000000;
}

/// @brief Return the predefined yellow color constant.
/// @return Packed `0x00FFFF00`.
int64_t rt_color_yellow(void) {
    return 0xFFFF00;
}

/// @brief Return the predefined cyan color constant.
/// @return Packed `0x0000FFFF`.
int64_t rt_color_cyan(void) {
    return 0x00FFFF;
}

/// @brief Return the predefined magenta color constant.
/// @return Packed `0x00FF00FF`.
int64_t rt_color_magenta(void) {
    return 0xFF00FF;
}

/// @brief Return the predefined gray color constant.
/// @return Packed `0x00808080`.
int64_t rt_color_gray(void) {
    return 0x808080;
}

/// @brief Return the predefined orange color constant.
/// @return Packed `0x00FFA500`.
int64_t rt_color_orange(void) {
    return 0xFFA500;
}

/// @brief Construct a color from red, green, blue components (0-255).
/// @param r Red channel, clamped to 0..255.
/// @param g Green channel, clamped to 0..255.
/// @param b Blue channel, clamped to 0..255.
/// @return Packed `0x00RRGGBB`.
int64_t rt_color_rgb(int64_t r, int64_t g, int64_t b) {
    uint8_t r8 = (r < 0) ? 0 : (r > 255) ? 255 : (uint8_t)r;
    uint8_t g8 = (g < 0) ? 0 : (g > 255) ? 255 : (uint8_t)g;
    uint8_t b8 = (b < 0) ? 0 : (b > 255) ? 255 : (uint8_t)b;
    return (int64_t)(((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8);
}

/// @brief Construct a color from red, green, blue, alpha components (0-255).
/// @param r Red channel, clamped to 0..255.
/// @param g Green channel, clamped to 0..255.
/// @param b Blue channel, clamped to 0..255.
/// @param a Alpha channel, clamped to 0..255.
/// @return Packed `0xAARRGGBB`.
int64_t rt_color_rgba(int64_t r, int64_t g, int64_t b, int64_t a) {
    uint8_t r8 = (r < 0) ? 0 : (r > 255) ? 255 : (uint8_t)r;
    uint8_t g8 = (g < 0) ? 0 : (g > 255) ? 255 : (uint8_t)g;
    uint8_t b8 = (b < 0) ? 0 : (b > 255) ? 255 : (uint8_t)b;
    uint8_t a8 = (a < 0) ? 0 : (a > 255) ? 255 : (uint8_t)a;
    return (int64_t)(((uint32_t)a8 << 24) | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) |
                     (uint32_t)b8);
}

/// @brief Construct a packed RGB color from HSL components.
///
/// Hue wraps modulo 360; saturation and lightness are clamped to 0..100.
///
/// @param h Hue in degrees.
/// @param s Saturation percentage.
/// @param l Lightness percentage.
/// @return Packed `0x00RRGGBB`.
int64_t rt_color_from_hsl(int64_t h, int64_t s, int64_t l) {
    h = ((h % 360) + 360) % 360;
    if (s < 0)
        s = 0;
    if (s > 100)
        s = 100;
    if (l < 0)
        l = 0;
    if (l > 100)
        l = 100;

    int64_t r, g, b;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Get the h value.
/// @param color
/// @return Result value.
int64_t rt_color_get_h(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    return h;
}

/// @brief Get the s value.
/// @param color
/// @return Result value.
int64_t rt_color_get_s(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    return s;
}

/// @brief Extract the lightness (L) component of a packed RGB color in the
///        HSL color space.
///
/// Real implementation — Color helpers operate purely on the integer
/// representation, so they remain functional in graphics-disabled builds.
/// Routes through the shared `rtg_rgb_to_hsl` helper.
///
/// @param color Packed 0xAARRGGBB color; alpha is ignored.
///
/// @return Lightness in 0..100 (percentage).
int64_t rt_color_get_l(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    return l;
}

/// @brief Linearly interpolate between two packed RGB colors.
///
/// Real implementation. Each channel is interpolated independently with
/// `t` clamped to 0..100. Alpha bytes are ignored — the result has alpha 0.
///
/// @param c1 Start color, packed 0xAARRGGBB.
/// @param c2 End color, packed 0xAARRGGBB.
/// @param t  Interpolation parameter as a percentage (0 = c1, 100 = c2);
///           clamped before use.
///
/// @return Interpolated color, packed 0x00RRGGBB.
int64_t rt_color_lerp(int64_t c1, int64_t c2, int64_t t) {
    if (t < 0)
        t = 0;
    if (t > 100)
        t = 100;

    int64_t r1 = (c1 >> 16) & 0xFF;
    int64_t g1 = (c1 >> 8) & 0xFF;
    int64_t b1 = c1 & 0xFF;
    int64_t r2 = (c2 >> 16) & 0xFF;
    int64_t g2 = (c2 >> 8) & 0xFF;
    int64_t b2 = c2 & 0xFF;

    int64_t r = r1 + (r2 - r1) * t / 100;
    int64_t g = g1 + (g2 - g1) * t / 100;
    int64_t b = b1 + (b2 - b1) * t / 100;
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Extract the red byte from a packed 0xAARRGGBB color.
///
/// @param color Packed color.
///
/// @return Red component in 0..255.
int64_t rt_color_get_r(int64_t color) {
    return (color >> 16) & 0xFF;
}

/// @brief Extract the green byte from a packed 0xAARRGGBB color.
///
/// @param color Packed color.
///
/// @return Green component in 0..255.
int64_t rt_color_get_g(int64_t color) {
    return (color >> 8) & 0xFF;
}

/// @brief Extract the blue byte from a packed 0xAARRGGBB color.
///
/// @param color Packed color.
///
/// @return Blue component in 0..255.
int64_t rt_color_get_b(int64_t color) {
    return color & 0xFF;
}

/// @brief Extract the alpha byte from a packed 0xAARRGGBB color.
///
/// @param color Packed color.
///
/// @return Alpha component in 0..255 (0 = fully transparent, 255 = opaque).
int64_t rt_color_get_a(int64_t color) {
    if (((uint64_t)color & (uint64_t)RT_COLOR_EXPLICIT_ALPHA_FLAG) == 0 &&
        (((uint64_t)color & 0xFFFFFFFFu) <= 0x00FFFFFFu))
        return 255;
    return (color >> 24) & 0xFF;
}

/// @brief Brighten an RGB color by interpolating each channel toward 255.
///
/// Real implementation. Each channel is shifted `amount`% of the way toward
/// white. `amount = 0` returns the original color; `amount = 100` returns
/// pure white. Operates per-channel — does not preserve hue under saturation.
///
/// @param color  Packed 0xAARRGGBB color; alpha is dropped from the result.
/// @param amount Brightening percentage 0..100; clamped before use.
///
/// @return Brightened color, packed 0x00RRGGBB.
int64_t rt_color_brighten(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;

    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;

    r = r + (255 - r) * amount / 100;
    g = g + (255 - g) * amount / 100;
    b = b + (255 - b) * amount / 100;
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Darken an RGB color by interpolating each channel toward 0.
///
/// Real implementation, mirror of `rt_color_brighten`. `amount = 0` returns
/// the original color; `amount = 100` returns pure black.
///
/// @param color  Packed 0xAARRGGBB color; alpha is dropped from the result.
/// @param amount Darkening percentage 0..100; clamped before use.
///
/// @return Darkened color, packed 0x00RRGGBB.
int64_t rt_color_darken(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;

    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;

    r = r - r * amount / 100;
    g = g - g * amount / 100;
    b = b - b * amount / 100;
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Parse a CSS-style hex color string into a packed integer.
///
/// Real implementation. Accepts an optional leading `#` and three forms:
///   - 6-digit `RRGGBB` → returns `0x00RRGGBB`
///   - 8-digit `RRGGBBAA` → returns `0xAARRGGBB` (alpha re-positioned)
///   - 3-digit `RGB` shorthand → expanded to `0x00RRGGBB` by duplicating each
///     nibble (e.g. `#F0A` → `0x00FF00AA`)
/// Any other length is parsed as raw hex without further reinterpretation.
///
/// @param hex Source string. NULL or non-hex content returns 0.
///
/// @return Packed color value (see length-specific layout above), or 0.
int64_t rt_color_from_hex(rt_string hex) {
    const char *s = rt_string_cstr(hex);
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    size_t len = strlen(s);
    unsigned long val = strtoul(s, NULL, 16);
    if (len == 6)
        return (int64_t)val;
    if (len == 8) {
        int64_t r = (val >> 24) & 0xFF;
        int64_t g = (val >> 16) & 0xFF;
        int64_t b = (val >> 8) & 0xFF;
        int64_t a = val & 0xFF;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    if (len == 3) {
        int64_t r = (val >> 8) & 0xF;
        int64_t g = (val >> 4) & 0xF;
        int64_t b = val & 0xF;
        return ((r | (r << 4)) << 16) | ((g | (g << 4)) << 8) | (b | (b << 4));
    }
    return (int64_t)val;
}

/// @brief Format a packed color as an uppercase CSS-style hexadecimal string.
///
/// Colors with a non-zero alpha byte use `#RRGGBBAA`; colors with a zero
/// alpha byte use `#RRGGBB`.
///
/// @param color Packed `0xAARRGGBB` color.
/// @return An owned runtime string containing the formatted color.
rt_string rt_color_to_hex(int64_t color) {
    char buf[10];
    int64_t a = (color >> 24) & 0xFF;
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int len;
    if (a != 0)
        len = snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", (int)r, (int)g, (int)b, (int)a);
    else
        len = snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)r, (int)g, (int)b);
    return rt_string_from_bytes(buf, (size_t)len);
}

/// @brief Increase the saturation of an RGB color via HSL round-trip.
///
/// Real implementation. Converts to HSL, adds `amount` to the saturation
/// channel (clamping at 100), then converts back to RGB. Hue and lightness
/// are preserved exactly. `amount = 0` returns the input unchanged;
/// `amount = 100` produces fully saturated output.
///
/// @param color  Packed 0xAARRGGBB color; alpha is dropped from the result.
/// @param amount Saturation delta as a percentage (0..100); clamped.
///
/// @return Saturated color, packed 0x00RRGGBB.
int64_t rt_color_saturate(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    s += amount;
    if (s > 100)
        s = 100;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Decrease the saturation of an RGB color via HSL round-trip.
///
/// Real implementation, mirror of `rt_color_saturate`. `amount = 100`
/// reduces the color to a pure gray (saturation 0).
///
/// @param color  Packed 0xAARRGGBB color; alpha is dropped from the result.
/// @param amount Saturation delta as a percentage (0..100); clamped.
///
/// @return Desaturated color, packed 0x00RRGGBB.
int64_t rt_color_desaturate(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    s -= amount;
    if (s < 0)
        s = 0;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Compute the hue-complement of an RGB color (180° hue rotation).
///
/// Real implementation. Used by UI palettes to derive a high-contrast
/// accent from a primary color. Saturation and lightness are preserved.
///
/// @param color Packed 0xAARRGGBB color; alpha is dropped from the result.
///
/// @return Complementary color, packed 0x00RRGGBB.
int64_t rt_color_complement(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    h = (h + 180) % 360;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}

/// @brief Convert an RGB color to grayscale using ITU-R BT.601 luma weights.
///
/// Real implementation. Applies the standard NTSC/JPEG luma formula
/// `Y = 0.299*R + 0.587*G + 0.114*B`, broadcasts the resulting brightness
/// to all three channels, and returns the packed gray.
///
/// @param color Packed 0xAARRGGBB color; alpha is dropped from the result.
///
/// @return Gray color, packed 0x00YYYYYY where YY = computed luma.
int64_t rt_color_grayscale(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t gray = (r * 299 + g * 587 + b * 114) / 1000;
    return ((gray & 0xFF) << 16) | ((gray & 0xFF) << 8) | (gray & 0xFF);
}

/// @brief Bitwise-invert each RGB channel of a packed color.
///
/// Real implementation. Each channel is replaced by `255 - channel`. Alpha
/// is dropped. Note: this is per-channel inversion, not hue rotation —
/// for the latter use `rt_color_complement`.
///
/// @param color Packed 0xAARRGGBB color; alpha is dropped from the result.
///
/// @return Inverted color, packed 0x00RRGGBB.
int64_t rt_color_invert(int64_t color) {
    int64_t r = 255 - ((color >> 16) & 0xFF);
    int64_t g = 255 - ((color >> 8) & 0xFF);
    int64_t b = 255 - (color & 0xFF);
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
}
