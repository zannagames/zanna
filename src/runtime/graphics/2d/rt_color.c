//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_color.c
/// @file
/// @brief Implements allocation-free color transforms plus runtime-string hex
///        parsing and formatting.
// Purpose: Color utilities for the 2D graphics API: HSL<->RGB conversion, component
//   getters, lerp, brighten/darken, saturate/desaturate, complement,
//   grayscale, invert, and hex parse/format. Pure color math (no canvas).
//
// Color representation:
//   - Plain RGB values occupy 0xRRGGBB and imply full opacity when rendered.
//   - Color.RGBA values occupy 0xAARRGGBB and carry
//     RT_COLOR_EXPLICIT_ALPHA_FLAG outside the component bytes.
//   - Transform operations preserve explicit-alpha intent; if either endpoint
//     of a lerp is explicit, the result is explicit.
//
// Ownership/Lifetime:
//   - Numeric color helpers allocate nothing. Hex input strings are borrowed;
//     rt_color_to_hex() returns a newly owned runtime string.
//
// Links: src/runtime/graphics/common/rt_graphics.h (public rt_color_* API),
//        rt_graphics2d.h,
//        rt_drawing_advanced.c (drawing primitives that consume colors)
//
//===----------------------------------------------------------------------===//

#include "rt_graphics2d.h"
#include "rt_graphics_internal.h"
#include "rt_heap.h"

#include <limits.h>

#ifdef ZANNA_ENABLE_GRAPHICS

//=============================================================================
// Extended Color Functions
//=============================================================================

/// @brief Clamp a runtime color channel to the 8-bit component range.
/// @details Public color helpers receive arbitrary int64 values. Clamping
///          prevents out-of-range channels from wrapping to unrelated colors.
/// @param value Component value to clamp.
/// @return Saturated component in [0, 255].
static int64_t rt_color_clamp_channel(int64_t value) {
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return value;
}

/// @brief Build an RGB color from hue, saturation, and lightness.
/// @details Hue wraps onto the 0..359 color wheel. Saturation and lightness
///          are percentages and clamp to [0, 100] before conversion.
/// @param h Hue in degrees; arbitrary integers wrap modulo 360.
/// @param s Saturation percentage, clamped to 0..100.
/// @param l Lightness percentage, clamped to 0..100.
/// @return Plain implicit-alpha `0xRRGGBB` runtime color.
int64_t rt_color_from_hsl(int64_t h, int64_t s, int64_t l) {
    // Clamp inputs
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

    return (rt_color_clamp_channel(r) << 16) | (rt_color_clamp_channel(g) << 8) |
           rt_color_clamp_channel(b);
}

/// @brief Convert a color's RGB payload to its HSL hue.
/// @param color Runtime color whose red, green, and blue bytes are read.
/// @return Hue in degrees in the converter's 0..359 range.
int64_t rt_color_get_h(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    return h;
}

/// @brief Convert a color's RGB payload to its HSL saturation.
/// @param color Runtime color whose red, green, and blue bytes are read.
/// @return Saturation percentage in 0..100.
int64_t rt_color_get_s(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    return s;
}

/// @brief Convert a color's RGB payload to its HSL lightness.
/// @param color Runtime color whose red, green, and blue bytes are read.
/// @return Lightness percentage in 0..100.
int64_t rt_color_get_l(int64_t color) {
    int64_t r = (color >> 16) & 0xFF;
    int64_t g = (color >> 8) & 0xFF;
    int64_t b = color & 0xFF;
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    return l;
}

/// @brief Test the runtime "explicit alpha" tag bit on a 64-bit color value.
/// @details Color literals built via Color.RGBA(...) carry RT_COLOR_EXPLICIT_ALPHA_FLAG
///          (bit 56) so downstream transforms (Brighten, Darken, Lerp, …) can
///          tell user-supplied alpha apart from "no alpha specified, treat as
///          opaque". Plain Color.RGB(...) values do NOT have this flag.
/// @param color Runtime packed color.
/// @return `1` if the color carries user-specified alpha; otherwise `0`.
static int8_t rt_color_has_explicit_alpha(int64_t color) {
    return (color & RT_COLOR_EXPLICIT_ALPHA_FLAG) != 0;
}

/// @brief Decompose a tagged color value into r/g/b/a components plus the alpha-tag flag.
/// @details Reads bytes 0-2 as B/G/R, byte 3 as A (or 255 when the explicit-alpha
///          flag is unset), and exposes the flag itself so callers can re-pack
///          the result through rt_color_pack_rgba_like and preserve user intent.
///          Each out-pointer is individually NULL-safe (skipped if NULL).
/// @param color Runtime tagged/plain color to decompose.
/// @param r Optional output for the red channel.
/// @param g Optional output for the green channel.
/// @param b Optional output for the blue channel.
/// @param a Optional output for explicit alpha or 255 when alpha is implicit.
/// @param has_alpha Optional output for the explicit-alpha flag.
static void rt_color_split_rgba(
    int64_t color, int64_t *r, int64_t *g, int64_t *b, int64_t *a, int8_t *has_alpha) {
    int8_t explicit_alpha = rt_color_has_explicit_alpha(color);
    if (r)
        *r = (color >> 16) & 0xFF;
    if (g)
        *g = (color >> 8) & 0xFF;
    if (b)
        *b = color & 0xFF;
    if (a)
        *a = explicit_alpha ? ((color >> 24) & 0xFF) : 255;
    if (has_alpha)
        *has_alpha = explicit_alpha;
}

/// @brief Re-pack r/g/b/a back into a tagged color value, preserving the explicit-alpha tag.
/// @details The inverse of rt_color_split_rgba. When @p has_alpha is non-zero the
///          result carries RT_COLOR_EXPLICIT_ALPHA_FLAG so downstream transforms
///          continue to honor the user-provided alpha; when zero, the alpha
///          byte is dropped and a plain RGB value is returned. Components are
///          clamped to 8 bits instead of wrapping.
/// @param r Red component.
/// @param g Green component.
/// @param b Blue component.
/// @param a Alpha component used only when @p has_alpha is nonzero.
/// @param has_alpha Nonzero to emit tagged explicit alpha.
/// @return Tagged `0xAARRGGBB` color or plain `0xRRGGBB` color.
static int64_t rt_color_pack_rgba_like(
    int64_t r, int64_t g, int64_t b, int64_t a, int8_t has_alpha) {
    r = rt_color_clamp_channel(r);
    g = rt_color_clamp_channel(g);
    b = rt_color_clamp_channel(b);
    a = rt_color_clamp_channel(a);
    int64_t rgb = (r << 16) | (g << 8) | b;
    if (!has_alpha)
        return rgb;
    return ((a << 24) | rgb) | RT_COLOR_EXPLICIT_ALPHA_FLAG;
}

/// @brief Linearly interpolate two runtime colors by an integer percentage.
/// @details Clamps @p t to 0..100 and interpolates every RGB channel using
///          truncating integer division. Implicit alpha is treated as 255;
///          the result is explicitly tagged when either input is explicit.
/// @param c1 Color returned at zero percent.
/// @param c2 Color returned at 100 percent.
/// @param t Interpolation percentage.
/// @return Interpolated packed color with preserved alpha intent.
int64_t rt_color_lerp(int64_t c1, int64_t c2, int64_t t) {
    if (t < 0)
        t = 0;
    if (t > 100)
        t = 100;

    int64_t r1 = 0, g1 = 0, b1 = 0, a1 = 255;
    int64_t r2 = 0, g2 = 0, b2 = 0, a2 = 255;
    int8_t alpha1 = 0, alpha2 = 0;
    rt_color_split_rgba(c1, &r1, &g1, &b1, &a1, &alpha1);
    rt_color_split_rgba(c2, &r2, &g2, &b2, &a2, &alpha2);

    int64_t r = r1 + (r2 - r1) * t / 100;
    int64_t g = g1 + (g2 - g1) * t / 100;
    int64_t b = b1 + (b2 - b1) * t / 100;
    int64_t a = a1 + (a2 - a1) * t / 100;

    return rt_color_pack_rgba_like(r, g, b, a, alpha1 || alpha2);
}

/// @brief Extract the stored red byte.
/// @param color Runtime `0xAARRGGBB` or `0xRRGGBB` color.
/// @return Red channel in 0..255.
int64_t rt_color_get_r(int64_t color) {
    return (color >> 16) & 0xFF;
}

/// @brief Extract the stored green byte.
/// @param color Runtime `0xAARRGGBB` or `0xRRGGBB` color.
/// @return Green channel in 0..255.
int64_t rt_color_get_g(int64_t color) {
    return (color >> 8) & 0xFF;
}

/// @brief Extract the stored blue byte.
/// @param color Runtime `0xAARRGGBB` or `0xRRGGBB` color.
/// @return Blue channel in 0..255.
int64_t rt_color_get_b(int64_t color) {
    return color & 0xFF;
}

/// @brief Get the stored alpha byte from a color value.
/// @details Public color accessors preserve the historical packed-value contract:
///          `rgb(r,g,b)` has no stored alpha byte, so this returns 0 for RGB inputs.
///          Rendering helpers that need effective opacity use `rt_color_split_rgba`
///          internally and still treat RGB colors as opaque.
/// @param color Runtime packed color.
/// @return Stored alpha channel in 0..255, or `0` when alpha is implicit.
int64_t rt_color_get_a(int64_t color) {
    if (!rt_color_has_explicit_alpha(color))
        return 0;
    return (color >> 24) & 0xFF;
}

/// @brief Move each RGB channel toward white by an integer percentage.
/// @details Clamps @p amount to 0..100, preserves alpha unchanged, and uses
///          truncating integer arithmetic.
/// @param color Runtime color to transform.
/// @param amount Percentage of each channel's remaining distance to 255.
/// @return Brightened color with the input's explicit-alpha status preserved.
int64_t rt_color_brighten(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;

    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);

    // Increase each channel toward 255
    r = r + (255 - r) * amount / 100;
    g = g + (255 - g) * amount / 100;
    b = b + (255 - b) * amount / 100;

    return rt_color_pack_rgba_like(r, g, b, a, has_alpha);
}

/// @brief Move each RGB channel toward black by an integer percentage.
/// @details Clamps @p amount to 0..100, preserves alpha unchanged, and uses
///          truncating integer arithmetic.
/// @param color Runtime color to transform.
/// @param amount Percentage of each channel removed.
/// @return Darkened color with the input's explicit-alpha status preserved.
int64_t rt_color_darken(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;

    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);

    // Decrease each channel toward 0
    r = r - r * amount / 100;
    g = g - g * amount / 100;
    b = b - b * amount / 100;

    return rt_color_pack_rgba_like(r, g, b, a, has_alpha);
}

/// @brief Convert a single hex character ('0'-'9', 'a'-'f', 'A'-'F') to its 0-15 value; returns -1
/// on invalid input.
/// @param c ASCII character to decode.
/// @return Nibble value in 0..15, or `-1` when @p c is not hexadecimal.
static int rt_color_hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// @brief Parse exactly `len` hex characters from `s` into `*out`; returns 1 on success, 0 on
/// invalid char or NULL input.
/// @param s Borrowed byte sequence containing at least @p len characters.
/// @param len Exact number of hexadecimal digits to consume.
/// @param out Required output receiving the accumulated unsigned value.
/// @return `1` on success; `0` for null pointers or a non-hexadecimal digit.
static int rt_color_parse_hex_n(const char *s, size_t len, uint64_t *out) {
    if (!s || !out)
        return 0;

    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        int digit = rt_color_hex_digit(s[i]);
        if (digit < 0)
            return 0;
        value = (value << 4) | (uint64_t)digit;
    }

    *out = value;
    return 1;
}

/// @brief Parse a CSS-style hexadecimal runtime color.
/// @details Accepts an optional leading `#` followed by `RGB`, `RRGGBB`, or
///          `RRGGBBAA`. Three-digit input duplicates each nibble. Eight-digit
///          input is rearranged into tagged runtime `AARRGGBB` form; shorter
///          forms remain implicit-alpha RGB.
/// @param hex Borrowed runtime string to parse.
/// @return Parsed packed color, or `0` for null, empty, unsupported-length, or
///         non-hexadecimal input. This fallback is indistinguishable from black.
int64_t rt_color_from_hex(rt_string hex) {
    if (!hex)
        return 0;
    const char *s = rt_string_cstr(hex);
    if (!s)
        return 0;
    int64_t raw_len = rt_str_len(hex);
    if (raw_len <= 0)
        return 0;
    size_t offset = (s[0] == '#') ? 1u : 0u;
    if ((uint64_t)raw_len < offset)
        return 0;
    s += offset;
    size_t len = (size_t)((uint64_t)raw_len - offset);
    uint64_t val = 0;
    if (len == 6) {
        if (!rt_color_parse_hex_n(s, len, &val))
            return 0;
        return (int64_t)val; // 0xRRGGBB
    }
    if (len == 8) {
        if (!rt_color_parse_hex_n(s, len, &val))
            return 0;
        // Input is RRGGBBAA, store as AARRGGBB
        int64_t r = (val >> 24) & 0xFF;
        int64_t g = (val >> 16) & 0xFF;
        int64_t b = (val >> 8) & 0xFF;
        int64_t a = val & 0xFF;
        int64_t packed = (a << 24) | (r << 16) | (g << 8) | b;
        return packed | RT_COLOR_EXPLICIT_ALPHA_FLAG;
    }
    if (len == 3) {
        if (!rt_color_parse_hex_n(s, len, &val))
            return 0;
        // Shorthand: RGB -> RRGGBB
        int64_t r = (val >> 8) & 0xF;
        int64_t g = (val >> 4) & 0xF;
        int64_t b = val & 0xF;
        return ((r | (r << 4)) << 16) | ((g | (g << 4)) << 8) | (b | (b << 4));
    }
    return 0;
}

/// @brief Format a runtime color as uppercase CSS hexadecimal text.
/// @details Tagged explicit-alpha values emit `#RRGGBBAA`. Untagged values no
///          larger than `0xFFFFFF` emit `#RRGGBB`; larger untagged values are
///          interpreted as raw `0xRRGGBBAA` and also emit eight digits.
/// @param color Packed runtime or raw-RGBA color.
/// @return Newly owned formatted runtime string. Formatting failure returns an
///         owned empty string when allocation succeeds; allocation can return
///         `NULL`.
rt_string rt_color_to_hex(int64_t color) {
    char buf[10];
    /* Decode with the same 3-way convention the pixel pipeline uses
     * (rt_pixels_color_to_rgba): a tagged Color.RGBA is 0xAARRGGBB, an untagged
     * value <= 0x00FFFFFF is opaque 0x00RRGGBB, and any larger untagged value is
     * raw 0xRRGGBBAA. The previous code always assumed ARGB, so a hand-built raw
     * RGBA value (e.g. 0x11223344) was mis-serialized (channels rotated). */
    uint64_t c = (uint64_t)color;
    int64_t r, g, b, a;
    int emit_alpha;
    if ((c & (uint64_t)RT_COLOR_EXPLICIT_ALPHA_FLAG) != 0) {
        a = (int64_t)((c >> 24) & 0xFF);
        r = (int64_t)((c >> 16) & 0xFF);
        g = (int64_t)((c >> 8) & 0xFF);
        b = (int64_t)(c & 0xFF);
        emit_alpha = 1;
    } else if (c <= 0x00FFFFFFu) {
        r = (int64_t)((c >> 16) & 0xFF);
        g = (int64_t)((c >> 8) & 0xFF);
        b = (int64_t)(c & 0xFF);
        a = 255;
        emit_alpha = 0;
    } else {
        r = (int64_t)((c >> 24) & 0xFF);
        g = (int64_t)((c >> 16) & 0xFF);
        b = (int64_t)((c >> 8) & 0xFF);
        a = (int64_t)(c & 0xFF);
        emit_alpha = 1;
    }
    int len;
    if (emit_alpha)
        len = snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", (int)r, (int)g, (int)b, (int)a);
    else
        len = snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)r, (int)g, (int)b);
    if (len < 0 || (size_t)len >= sizeof(buf))
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(buf, (size_t)len);
}

/// @brief Increase HSL saturation by a fixed number of percentage points.
/// @details Clamps @p amount to 0..100 and the resulting saturation to 100;
///          lightness, hue, alpha, and explicit-alpha intent are preserved
///          subject to RGB/HSL conversion rounding.
/// @param color Runtime color to transform.
/// @param amount Saturation percentage points to add.
/// @return Saturated packed color.
int64_t rt_color_saturate(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;
    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    s = s + amount;
    if (s > 100)
        s = 100;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return rt_color_pack_rgba_like(r, g, b, a, has_alpha);
}

/// @brief Decrease HSL saturation by a fixed number of percentage points.
/// @details Clamps @p amount to 0..100 and the resulting saturation to zero;
///          lightness, hue, alpha, and explicit-alpha intent are preserved
///          subject to RGB/HSL conversion rounding.
/// @param color Runtime color to transform.
/// @param amount Saturation percentage points to subtract.
/// @return Desaturated packed color.
int64_t rt_color_desaturate(int64_t color, int64_t amount) {
    if (amount < 0)
        amount = 0;
    if (amount > 100)
        amount = 100;
    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    s = s - amount;
    if (s < 0)
        s = 0;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return rt_color_pack_rgba_like(r, g, b, a, has_alpha);
}

/// @brief Rotate a color's HSL hue by 180 degrees.
/// @details Saturation, lightness, alpha, and explicit-alpha intent are
///          preserved subject to RGB/HSL conversion rounding.
/// @param color Runtime color to transform.
/// @return Complementary packed color.
int64_t rt_color_complement(int64_t color) {
    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);
    int64_t h, s, l;
    rtg_rgb_to_hsl(r, g, b, &h, &s, &l);
    h = (h + 180) % 360;
    rtg_hsl_to_rgb(h, s, l, &r, &g, &b);
    return rt_color_pack_rgba_like(r, g, b, a, has_alpha);
}

/// @brief Convert RGB to integer luma while preserving alpha.
/// @details Uses `(299R + 587G + 114B) / 1000` and writes the truncated result
///          to all three channels.
/// @param color Runtime color to transform.
/// @return Grayscale packed color with explicit-alpha intent preserved.
int64_t rt_color_grayscale(int64_t color) {
    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);
    // Luminance formula: 0.299R + 0.587G + 0.114B
    int64_t gray = (r * 299 + g * 587 + b * 114) / 1000;
    return rt_color_pack_rgba_like(gray, gray, gray, a, has_alpha);
}

/// @brief Replace each RGB channel with its 255-complement.
/// @param color Runtime color to transform.
/// @return Inverted packed color with alpha and explicit-alpha intent unchanged.
int64_t rt_color_invert(int64_t color) {
    int64_t r = 0, g = 0, b = 0, a = 255;
    int8_t has_alpha = 0;
    rt_color_split_rgba(color, &r, &g, &b, &a, &has_alpha);
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
    return rt_color_pack_rgba_like(r, g, b, a, has_alpha);
}

#else
typedef int rt_color_disabled_tu_guard;
#endif /* ZANNA_ENABLE_GRAPHICS */
